#!/usr/bin/env python3
"""Packaging must reject stale sources and unverified binaries before writing."""
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
spec = importlib.util.spec_from_file_location('release_package', ROOT / 'tools/release/release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class PackageIdentityTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='wqn-package-identity-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.build = self.root / 'build-test'
        self.build.mkdir()
        self.version = '20261012.4'
        self.record = self.root / 'dist' / self.version
        self.record.mkdir(parents=True)
        (self.build / 'app.bin').write_bytes(b'recorded firmware')
        (self.build / 'CMakeCache.txt').write_text('PROJECT_VER:STRING=' + self.version + '\n')
        (self.build / 'flasher_args.json').write_text(json.dumps({
            'app': {'offset': '0x20000', 'file': 'app.bin'}}))
        self.manifest = {'main/main.cpp': 'source-hash'}
        self.identity = {
            'version': self.version, 'profile': 'test', 'idf': '5.5.4',
            'fingerprint': release.V.fingerprint(self.manifest, 'test', '5.5.4'),
            'app': {'sha256': hashlib.sha256(b'recorded firmware').hexdigest()},
        }
        self.identity_path = self.record / 'identity.json'
        self.identity_path.write_text(json.dumps(self.identity))
        self.cli = SimpleNamespace(build_dir='build-test')
        for module, name, value in (
            (release, 'resolve_build_dir', lambda unused: self.build),
            (release, 'release_dir', lambda version: self.root / 'dist' / version),
            (release.V, 'source_manifest', lambda: self.manifest),
            # Packaging a recorded image needs no current IDF environment.
            (release.V, 'idf_version', lambda: 'unknown'),
        ):
            seam = patch.object(module, name, value)
            seam.start()
            self.addCleanup(seam.stop)
        self.effects = {}
        for name in ('collect_identity', 'write_record', 'ensure_esptool', 'run'):
            seam = patch.object(release, name)
            self.effects[name] = seam.start()
            self.addCleanup(seam.stop)
        self.effects['write_record'].return_value = self.record
        seam = patch.object(release.V, 'write_source_snapshot')
        self.effects['snapshot'] = seam.start()
        self.addCleanup(seam.stop)
        self.zip = self.record / ('WQN-Note4-Flasher-' + self.version + '.zip')
        self.zip.write_bytes(b'packaged fixture')

    def assert_rejected_before_side_effects(self):
        before = self.identity_path.read_bytes() if self.identity_path.exists() else None
        with self.assertRaises(SystemExit):
            release.cmd_package(self.cli)
        for effect in self.effects.values():
            effect.assert_not_called()
        self.assertEqual(self.identity_path.read_bytes() if self.identity_path.exists() else None,
                         before)

    def test_changed_sources_cannot_relabel_old_image(self):
        self.manifest['main/main.cpp'] = 'new-source-hash'
        self.assert_rejected_before_side_effects()

    def test_changed_dependency_lock_is_rejected(self):
        self.manifest['dependencies.lock'] = 'new-component-versions'
        self.assert_rejected_before_side_effects()

    def test_changed_binary_is_rejected(self):
        (self.build / 'app.bin').write_bytes(b'unrecorded rebuilt firmware')
        self.assert_rejected_before_side_effects()

    def test_missing_record_is_rejected(self):
        self.identity_path.unlink()
        self.assert_rejected_before_side_effects()

    def test_different_profile_is_rejected(self):
        self.cli.build_dir = 'build-other-profile'
        self.assert_rejected_before_side_effects()

    def test_matching_image_keeps_baked_version(self):
        self.assertEqual(release.cmd_package(self.cli), 0)
        self.effects['collect_identity'].assert_not_called()
        info = self.effects['write_record'].call_args.args[0]
        self.assertEqual(info['version'], self.version)
        self.assertEqual(info['fingerprint'], self.identity['fingerprint'])
        self.assertEqual(info['kind'], 'release')
        command = self.effects['run'].call_args.args[0]
        self.assertEqual(command[command.index('--release-version') + 1], self.version)


if __name__ == '__main__':
    unittest.main()
