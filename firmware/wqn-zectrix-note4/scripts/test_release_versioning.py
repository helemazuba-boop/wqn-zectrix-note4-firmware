#!/usr/bin/env python3
"""Release fingerprint regressions using temporary build inputs."""
import importlib.util
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('release_versioning', ROOT / 'tools/release/versioning.py')
versioning = importlib.util.module_from_spec(spec)
spec.loader.exec_module(versioning)


class ReleaseManifestTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='wqn-release-manifest-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for name, value in {
            'dependencies.lock': 'component: version-1\n',
            'sdkconfig': 'CONFIG_TEST=y\n',
            'runtime.lock': 'process-lock\n',
        }.items():
            (self.root / name).write_text(value)
        for name, value in {
            'PROJECT_DIR': self.root,
            'repo_root': lambda: self.root,
            'project_relpath': lambda: '.',
            'git': lambda *args, **kwargs: (
                'dependencies.lock\0runtime.lock\0' if '--others' not in args else ''),
        }.items():
            seam = patch.object(versioning, name, value)
            seam.start()
            self.addCleanup(seam.stop)

    def test_dependency_change_changes_fingerprint(self):
        before = versioning.source_manifest()
        (self.root / 'dependencies.lock').write_text('component: version-2\n')
        after = versioning.source_manifest()
        self.assertIn('dependencies.lock', before)
        self.assertNotEqual(versioning.fingerprint(before, 'test', '5.5'),
                            versioning.fingerprint(after, 'test', '5.5'))

    def test_transient_locks_do_not_change_fingerprint(self):
        before = versioning.source_manifest()
        (self.root / 'runtime.lock').write_text('another-process\n')
        self.assertNotIn('runtime.lock', before)
        self.assertEqual(before, versioning.source_manifest())

    def test_source_snapshot_contains_dependency_lock(self):
        target = self.root / 'snapshot.tgz'
        versioning.write_source_snapshot(target)
        with tarfile.open(target) as archive:
            self.assertEqual(archive.extractfile('dependencies.lock').read(),
                             (self.root / 'dependencies.lock').read_bytes())


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *sys.argv[1:]])
