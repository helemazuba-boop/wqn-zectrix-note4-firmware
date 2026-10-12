#!/usr/bin/env python3
"""Publish preflight and immutable release regressions; no real SSH or uploads."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/release'))
spec = importlib.util.spec_from_file_location('release_publish', ROOT / 'tools/release/release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class PublishTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='wqn-release-publish-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.build = self.root / 'build-test'
        self.build.mkdir()
        self.version = '20261012.1'
        self.record = self.root / 'dist' / self.version
        self.record.mkdir(parents=True)
        (self.build / 'app.bin').write_bytes(b'firmware')
        (self.build / 'CMakeCache.txt').write_text('PROJECT_VER:STRING=' + self.version + '\n')
        (self.build / 'flasher_args.json').write_text(json.dumps({
            'app': {'offset': '0x20000', 'file': 'app.bin'}}))
        self.identity = {
            'version': self.version, 'fingerprint': 'current', 'base_commit': 'commit',
            'dirty': False, 'profile': 'test', 'idf': '5.5.4',
            'app': {'sha256': hashlib.sha256(b'firmware').hexdigest()},
        }
        (self.record / 'identity.json').write_text(json.dumps(self.identity))
        self.zip = self.record / ('WQN-Note4-Flasher-' + self.version + '.zip')
        self.zip.write_bytes(b'zip')
        self.zip_sha = release.V.sha256_file(self.zip)
        (self.record / 'UPDATELOG.md').write_text('## ' + self.version + ' fixture\n')
        self.index = {'days': {}}
        self.cli = SimpleNamespace(build_dir='build-test', ssh_host='fixture', remote_base='/releases')
        self.events = []
        self.original_loader = release.load_remote_index
        self.original_collector = release.collect_identity
        for name, value in {
            'resolve_build_dir': lambda unused: self.build,
            'release_dir': lambda version: self.root / 'dist' / version,
        }.items():
            seam = patch.object(release, name, value)
            seam.start()
            self.addCleanup(seam.stop)
        self.effects = {}
        for name in ('run', 'scp_to', 'store_remote_file', 'collect_identity',
                     'load_remote_index', 'ssh_capture'):
            seam = patch.object(release, name)
            self.effects[name] = seam.start()
            self.addCleanup(seam.stop)
        self.effects['load_remote_index'].side_effect = self.load_index
        self.effects['run'].side_effect = lambda cmd, **kwargs: self.events.append('mkdir')
        self.effects['scp_to'].side_effect = lambda *args: self.events.append('upload')
        self.effects['ssh_capture'].return_value = self.zip_sha + ' archive.zip'

    def load_index(self, *args):
        self.events.append('read-index')
        return json.loads(json.dumps(self.index))

    def existing(self, fingerprint='current', zip_sha=None):
        self.index = {'days': {'20261012': {'1': {
            'fingerprint': fingerprint, 'zip_sha256': zip_sha or self.zip_sha}}}}

    def assert_no_remote_writes(self):
        for name in ('run', 'scp_to', 'store_remote_file', 'collect_identity'):
            self.effects[name].assert_not_called()

    def test_conflict_is_rejected_before_any_remote_write(self):
        self.existing(fingerprint='another-image')
        with self.assertRaisesRegex(SystemExit, 'run `release`'):
            release.cmd_publish(self.cli)
        self.assert_no_remote_writes()

    def test_changed_package_cannot_overwrite_same_fingerprint(self):
        self.existing(zip_sha='different-package')
        with self.assertRaisesRegex(SystemExit, 'immutable'):
            release.cmd_publish(self.cli)
        self.assert_no_remote_writes()

    def test_identical_publish_preserves_remote_files(self):
        self.existing()
        self.assertEqual(release.cmd_publish(self.cli), 0)
        self.assert_no_remote_writes()

    def test_index_is_read_before_reserving_and_uploading(self):
        self.assertEqual(release.cmd_publish(self.cli), 0)
        self.assertEqual(self.events[:3], ['read-index', 'mkdir', 'upload'])
        command = self.effects['run'].call_args.args[0][-1]
        self.assertIn('&& mkdir /releases/v/' + self.version, command)
        self.assertNotIn('mkdir -p /releases/v/' + self.version, command)
        self.effects['collect_identity'].assert_not_called()
        index_write = self.effects['store_remote_file'].call_args_list[0].args
        published = json.loads(index_write[3])['days']['20261012']['1']
        self.assertEqual(published['app'], self.identity['app'])
        self.assertEqual(published['zip_sha256'], self.zip_sha)

    def test_existing_unindexed_directory_stops_uploads(self):
        self.effects['run'].side_effect = subprocess.CalledProcessError(1, ['ssh'])
        with self.assertRaises(subprocess.CalledProcessError):
            release.cmd_publish(self.cli)
        self.effects['scp_to'].assert_not_called()
        self.effects['store_remote_file'].assert_not_called()

    def test_remote_read_failure_stops_publish(self):
        self.effects['load_remote_index'].side_effect = subprocess.CalledProcessError(255, ['ssh'])
        with self.assertRaises(subprocess.CalledProcessError):
            release.cmd_publish(self.cli)
        self.assert_no_remote_writes()

    def test_new_remote_entries_survive_the_upload(self):
        later = {'days': {'20261012': {'2': {'fingerprint': 'other', 'kind': 'release'}}}}
        self.effects['load_remote_index'].side_effect = [{'days': {}}, later]
        self.assertEqual(release.cmd_publish(self.cli), 0)
        index_write = self.effects['store_remote_file'].call_args_list[0].args
        day = json.loads(index_write[3])['days']['20261012']
        self.assertEqual(day['2']['fingerprint'], 'other')
        self.assertEqual(day['1']['fingerprint'], 'current')

    def test_build_uses_remote_index_to_allocate_baked_version(self):
        # Exercise cmd_build -> collect_identity -> assign_version with a
        # fresh local cache, while substituting only the real build commands.
        today = release.datetime.now().strftime('%Y%m%d')
        self.cli.kind = 'release'
        self.cli.remote_index = {'days': {today: {'7': {'fingerprint': 'other'}}}}
        with patch.object(release, 'collect_identity', spec=self.original_collector) as collect:
            # The production function was saved before installing the spy.
            collect.side_effect = self.original_collector
            with patch.object(release.V, 'VERSION_INDEX_PATH', self.root / 'index.json'), \
                    patch.object(release.V, 'source_manifest', return_value={'main.cpp': 'hash'}), \
                    patch.object(release.V, 'base_commit', return_value='commit'), \
                    patch.object(release.V, 'is_dirty', return_value=False), \
                    patch.object(release, 'run_architecture_gate'), \
                    patch.object(release, 'write_record') as record:
                self.assertEqual(release.cmd_build(self.cli), 0)
                self.assertEqual(record.call_args.args[0]['version'], today + '.8')

    def test_release_reads_index_before_build(self):
        with patch.object(release, 'cmd_build') as build, \
                patch.object(release, 'cmd_package') as package, \
                patch.object(release, 'cmd_publish', return_value=0):
            build.side_effect = lambda args: self.events.append('build')
            self.assertEqual(release.cmd_release(self.cli), 0)
            self.assertEqual(self.events, ['read-index', 'build'])
            self.assertEqual(self.cli.remote_index, self.index)
            package.assert_called_once()

    def test_release_does_not_build_when_index_is_unreadable(self):
        self.effects['load_remote_index'].side_effect = subprocess.CalledProcessError(255, ['ssh'])
        with patch.object(release, 'cmd_build') as build:
            with self.assertRaises(subprocess.CalledProcessError):
                release.cmd_release(self.cli)
            build.assert_not_called()

    def test_index_loader_propagates_ssh_failure(self):
        with patch.object(release.subprocess, 'run', side_effect=subprocess.CalledProcessError(255, ['ssh'])):
            with self.assertRaises(subprocess.CalledProcessError):
                self.original_loader('fixture', '/releases')

    def test_index_loader_rejects_corrupt_or_empty_index(self):
        for raw in ('', 'corrupt'):
            with self.subTest(raw=raw), patch.object(release.subprocess, 'run',
                    return_value=SimpleNamespace(stdout=raw)):
                with self.assertRaises(json.JSONDecodeError):
                    self.original_loader('fixture', '/releases')


if __name__ == '__main__':
    unittest.main()
