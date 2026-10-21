#!/usr/bin/env python3
"""Release fingerprint regressions using temporary build inputs."""
import importlib.util
import json
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


class VersionAllocationTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='wqn-release-index-')
        self.addCleanup(temporary.cleanup)
        self.index_path = Path(temporary.name) / 'version-index.json'
        seam = patch.object(versioning, 'VERSION_INDEX_PATH', self.index_path)
        seam.start()
        self.addCleanup(seam.stop)

    def assign(self, remote=None):
        return versioning.assign_version('current', base_commit='commit', dirty=False,
                                         kind='release', date='20261012', remote_index=remote)

    def remote(self, records):
        return {'days': {'20261012': records}}

    def test_fresh_cache_allocates_above_remote_serials(self):
        self.assertEqual(self.assign(self.remote({'5': {'fingerprint': 'other'}})),
                         ('20261012.6', False))

    def test_conflicting_cached_version_is_never_reselected(self):
        self.assertEqual(self.assign(), ('20261012.1', False))
        remote = self.remote({'1': {'fingerprint': 'other'}, '5': {'fingerprint': 'another'}})
        self.assertEqual(self.assign(remote), ('20261012.6', False))
        self.assertEqual(self.assign(remote), ('20261012.6', True))
        self.assertEqual(self.assign(), ('20261012.6', True))
        records = json.loads(self.index_path.read_text())['days']['20261012']
        self.assertEqual(records['1']['fingerprint'], 'current')
        self.assertTrue(records['1']['remote_conflict'])

    def test_same_content_reuses_remote_identity_and_timestamp(self):
        remote = self.remote({'3': {'fingerprint': 'current', 'created_at': 'original-time'}})
        self.assertEqual(self.assign(remote), ('20261012.3', True))
        self.assertEqual(versioning.version_record('20261012.3')['created_at'], 'original-time')

    def test_matching_cached_and_remote_identity_is_reused(self):
        self.assign()
        self.assertEqual(self.assign(self.remote({'1': {'fingerprint': 'current'}})),
                         ('20261012.1', True))

    def test_allocation_respects_higher_local_serial(self):
        self.index_path.write_text(json.dumps(self.remote({'9': {'fingerprint': 'local'}})))
        self.assertEqual(self.assign(self.remote({'5': {'fingerprint': 'remote'}})),
                         ('20261012.10', False))


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *sys.argv[1:]])
