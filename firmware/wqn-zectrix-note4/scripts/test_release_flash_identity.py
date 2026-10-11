#!/usr/bin/env python3
"""Flash ledger regression: bind entries to recorded binaries, not live sources."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/release'))
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=ROOT / 'tools/release/release.py')
args, unittest_args = parser.parse_known_args()
spec = importlib.util.spec_from_file_location('release_flash_fixture', args.source)
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class FlashIdentityTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='wqn-flash-identity-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.build = self.root / 'build'
        self.build.mkdir()
        self.dist = self.root / 'dist'
        self.version = '20261011.5'
        self.record = self.dist / self.version
        self.record.mkdir(parents=True)
        self.blob = b'fixture recorded firmware'
        self.digest = hashlib.sha256(self.blob).hexdigest()
        (self.build / 'app.bin').write_bytes(self.blob)
        (self.build / 'CMakeCache.txt').write_text('PROJECT_VER:STRING=' + self.version + '\n')
        (self.build / 'flasher_args.json').write_text(json.dumps({
            'app': {'offset': '0x20000', 'file': 'app.bin'}}))
        self.identity = {
            'version': self.version, 'fingerprint': 'recorded-fingerprint',
            'base_commit_short': 'dc523648', 'dirty': True,
            'app': {'sha256': self.digest}}
        self.save_record()
        self.cli = SimpleNamespace(build_dir='build', port='COM7', status='ok', machine='fixture')
        for name, implementation in {
            'resolve_build_dir': lambda unused: self.build,
            'dist_dir': lambda: self.dist,
            'release_dir': lambda version: self.dist / version,
            # Simulates a newer tree and a missing/different IDF environment.
            # Calling this during record-flash was the false-version bug.
            'collect_identity': lambda *unused: {
                **self.identity, 'version': '20261011.8',
                'fingerprint': 'live-source-fingerprint'},
        }.items():
            seam = patch.object(release, name, implementation)
            seam.start()
            self.addCleanup(seam.stop)

    def save_record(self):
        (self.record / 'identity.json').write_text(json.dumps(self.identity))

    def history(self):
        return self.dist / release.HISTORY_NAME

    def assert_rejected(self):
        with self.assertRaises(SystemExit):
            release.cmd_record_flash(self.cli)
        self.assertFalse(self.history().exists())

    def test_source_changes_do_not_relabel_binary(self):
        self.assertEqual(release.cmd_record_flash(self.cli), 0)
        entry = json.loads(self.history().read_text())
        self.assertEqual(entry['version'], self.version)
        self.assertEqual(entry['fingerprint'], 'recorded-fingerprint')
        self.assertEqual(entry['app_sha256'], self.digest)
        self.assertEqual(entry['record_dir'], 'dist/' + self.version)
        self.assertEqual(entry['base_commit'], 'dc523648')
        self.assertTrue(entry['dirty'])

    def test_failed_flash_keeps_attempted_artifact_identity(self):
        self.cli.status = 'failed'
        self.assertEqual(release.cmd_record_flash(self.cli), 0)
        entry = json.loads(self.history().read_text())
        self.assertEqual(entry['version'], self.version)
        self.assertEqual(entry['status'], 'failed')

    def test_unrecorded_version_is_rejected(self):
        (self.build / 'CMakeCache.txt').write_text('PROJECT_VER:STRING=20261011.8\n')
        self.assert_rejected()

    def test_changed_binary_is_rejected(self):
        (self.build / 'app.bin').write_bytes(b'new unrecorded firmware')
        self.assert_rejected()

    def test_missing_binary_is_rejected(self):
        (self.build / 'app.bin').unlink()
        self.assert_rejected()

    def test_missing_cache_is_rejected(self):
        (self.build / 'CMakeCache.txt').unlink()
        self.assert_rejected()

    def test_missing_recorded_digest_is_rejected(self):
        self.identity['app'] = None
        self.save_record()
        self.assert_rejected()

    def test_mismatched_record_version_is_rejected(self):
        self.identity['version'] = '20261011.8'
        self.save_record()
        self.assert_rejected()


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *unittest_args])
