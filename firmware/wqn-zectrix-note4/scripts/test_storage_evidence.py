#!/usr/bin/env python3
"""Reject misleading storage-log attribution, percentiles and diagnostic controls."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from analyze_storage_log import analyze, parse_log, statistics
from build_diagnostic_pair import compare_configs, disabled_config


def identity(build='1234abcd', boot=2, mode=1):
    return f'I (90) wqn_fault: FAULT:boot build={build} boot={boot} mem_diag={mode}\n'


def sample(request=1, start=10000, duration=12, result='ESP_OK', owner='save-word'):
    return (f'I (100) storage_service: storage transaction complete: request={request} owner={owner} '
            f'queue_wait_ms=3 elapsed_ms={duration} result={result} stack_free_bytes=8000 '
            f'clock=esp_timer exec_start_us={start} exec_end_us={start + duration * 1000}\n')


def capacity(start=100, end=200, used=750, result='ESP_OK'):
    return (f'I (99) wqn_storage: STORAGE:capacity query_start_us={start} query_end_us={end} '
            f'total_bytes=1000 used_bytes={used} result={result}\n')


class StorageLogTests(unittest.TestCase):
    def test_counts_errors_without_marking_normal_empty_query_as_failure(self):
        segments = parse_log([identity(), sample(), sample(2, 30000, result='ESP_ERR_NOT_FOUND'),
                              sample(3, 60000, result='ESP_FAIL')])
        self.assertEqual(len(segments), 1)
        s = segments[0]
        self.assertEqual((s['build'], s['boot'], s['mem_diag']), ('1234abcd', 2, 1))
        self.assertEqual(s['summary']['errors_excluding_not_found'], 1)
        self.assertEqual(s['summary']['elapsed_ms'], {'n': 3, 'max': 12})

    def test_late_header_does_not_relabel_earlier_work(self):
        s = parse_log([sample(), identity(), sample(2, 40000)])
        self.assertIsNone(s[0]['build'])
        self.assertEqual(s[1]['build'], '1234abcd')
        self.assertEqual([len(x['samples']) for x in s], [1, 1])

    def test_retained_record_is_never_current_identity(self):
        s = parse_log(['wqn_fault: FAULT:record build=old boot=99\n', sample()])
        self.assertIsNone(s[0]['build'])

    def test_repeated_dump_and_foreground_overtaking_do_not_split_boot(self):
        s = parse_log([identity(), sample(4), identity(), sample(2, 40000)])
        self.assertEqual(len(s), 1)
        self.assertEqual(len(s[0]['samples']), 2)

    def test_cold_reboot_with_repeated_boot_id_stays_separate(self):
        s = parse_log([identity(boot=1), sample(), 'ESP-ROM:esp32s3\n',
                       'rst:0x1 (POWERON)\n', 'I (10) boot: ESP-IDF v5.5.6\n', identity(boot=1), sample()])
        self.assertEqual([len(x['samples']) for x in s], [1, 1])

    def test_clock_regression_without_boot_log_loses_identity(self):
        s = parse_log([identity(), sample(20, 1000000), sample(1, 10000)])
        self.assertEqual(len(s), 2)
        self.assertIsNone(s[1]['build'])

    def test_invalid_identity_clears_binding(self):
        s = parse_log([identity(), sample(), 'wqn_fault: FAULT:boot build=bad\n', sample(2, 50000)])
        self.assertIsNone(s[-1]['build'])
        self.assertEqual(s[-1]['rejected_lines'], [3])

    def test_duplicate_complete_lines_not_double_counted(self):
        s = parse_log([identity(), sample(), sample()])[0]
        self.assertEqual(len(s['samples']), 1)
        self.assertEqual(s['duplicate_lines'], [3])

    def test_bad_and_torn_durations_rejected(self):
        lines = [sample().replace('elapsed_ms=12', 'elapsed_ms=-1'),
                 sample().replace('exec_end_us=22000', 'exec_end_us=9000'),
                 sample().replace('elapsed_ms=12', 'elapsed_ms=11'),
                 sample().replace('exec_end_us=22000', ''),
                 sample().replace('result=ESP_OK', ''),
                 sample().replace('owner=save-word', 'owner=/storage/secret')]
        s = parse_log(lines)[0]
        self.assertEqual(len(s['rejected_lines']), 6)
        self.assertEqual(s['summary']['count'], 0)

    def test_capacity_is_only_prior_observation_with_explicit_age(self):
        s = parse_log([identity(), capacity(), sample()])[0]
        self.assertEqual(s['capacity_observations'][0]['used_percent'], 75)
        self.assertEqual(s['samples'][0]['capacity_age_us'], 9800)
        self.assertNotIn('used_percent', s['samples'][0])

    def test_concurrent_failed_or_invalid_capacity_does_not_bind(self):
        for observation in [capacity(end=15000), capacity(result='ESP_FAIL'),
                            capacity(used=1001), capacity().replace('query_end_us=200', '')]:
            with self.subTest(observation=observation):
                s = parse_log([identity(), capacity(), observation, sample()])[0]
                self.assertNotIn('last_capacity_line', s['samples'][0])

    def test_legacy_ready_capacity_has_no_fake_esp_timer_age(self):
        s = parse_log(['wqn_storage: storage SPIFFS ready: total=1000 used=500 page_size=256\n',
                       sample()])[0]
        self.assertEqual(s['samples'][0]['last_capacity_line'], 1)
        self.assertNotIn('capacity_age_us', s['samples'][0])

    def test_abandoned_timeout_low_stack_are_events_not_completed_samples(self):
        lines = ['storage transaction abandoned before execution:',
                 'storage transaction timed out in queue:',
                 'storage task stack low:',
                 'storage transaction overran foreground budget; waiting for completion:']
        s = parse_log(['storage_service: ' + line for line in lines])[0]
        self.assertEqual(s['summary']['count'], 0)
        self.assertEqual(s['event_counts'], {'abandoned': 1, 'queue_timeout': 1,
                                           'low_stack': 1, 'running_overrun': 1})

    def test_percentiles_need_enough_samples_in_each_group(self):
        self.assertEqual(statistics([9] * 19), {'n': 19, 'max': 9})
        self.assertEqual(statistics(list(range(20)))['p50'], 9)
        self.assertNotIn('p95', statistics([1] * 199))
        self.assertEqual(statistics(list(range(200)))['p95'], 189)
        self.assertNotIn('p99', statistics([1] * 999))
        self.assertEqual(statistics(list(range(1000)))['p99'], 989)

    def test_unbound_and_mixed_owner_totals_never_publish_percentiles(self):
        records = [sample(i + 1, i * 30000) for i in range(20)]
        unbound = parse_log(records)[0]
        self.assertNotIn('p50', unbound['owners']['save-word']['elapsed_ms'])
        bound = parse_log([identity(), *records])[0]
        self.assertNotIn('p50', bound['summary']['elapsed_ms'])
        self.assertEqual(bound['owners']['save-word']['elapsed_ms']['p50'], 12)

    def test_identity_change_and_mode_change_split_samples(self):
        s = parse_log([identity(), sample(), identity(mode=0), sample(2, 40000),
                       identity(build='abcd1234', mode=0), sample(3, 70000)])
        self.assertEqual([x['mem_diag'] for x in s], [1, 0, 0])
        self.assertEqual([len(x['samples']) for x in s], [1, 1, 1])

    def test_real_cli_exports_and_keeps_files_separate(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            paths = [root / 'a.log', root / 'b.log']
            for path in paths:
                path.write_text('\x1b[32m' + identity() + sample() + '\x1b[0m')
            report = analyze(paths)
            self.assertEqual(len(report['segments']), 2)
            self.assertEqual(len(report['files'][0]['sha256']), 64)
            script = Path(__file__).with_name('analyze_storage_log.py')
            subprocess.run([sys.executable, str(script), *map(str, paths), '--json', str(root / 'r.json'),
                            '--csv', str(root / 'r.csv')], check=True, stdout=subprocess.DEVNULL)
            self.assertEqual(len(json.loads((root / 'r.json').read_text())['segments']), 2)
            self.assertEqual(len((root / 'r.csv').read_text().splitlines()), 3)


class ControlConfigTests(unittest.TestCase):
    ON = ('CONFIG_WQN_MEMORY_DIAGNOSTICS=y\nCONFIG_WQN_MEMORY_SAMPLE_SECONDS=30\n'
          'CONFIG_WQN_MEMORY_HISTORY_ENABLE=y\nCONFIG_WQN_MEMORY_HISTORY_HOURS=24\n'
          'CONFIG_PM_ENABLE=y\nCONFIG_WQN_WIFI_PASSWORD="private"\n')

    def test_disable_only_switch_before_kconfig_dependency_resolution(self):
        off = disabled_config(self.ON)
        self.assertEqual(compare_configs(self.ON, off), {
            'CONFIG_WQN_MEMORY_DIAGNOSTICS': {'on': 'y', 'off': 'n'}})
        self.assertIn('CONFIG_PM_ENABLE=y', off)

    def test_dependency_removal_is_allowed(self):
        off = disabled_config(self.ON)
        off = '\n'.join(line for line in off.splitlines() if 'CONFIG_WQN_MEMORY_' not in line or 'DIAGNOSTICS' in line)
        self.assertEqual(len(compare_configs(self.ON, off)), 4)

    def test_reject_unrelated_drift_without_printing_secret(self):
        for old, new in [('CONFIG_PM_ENABLE=y', '# CONFIG_PM_ENABLE is not set'),
                         ('"private"', '"new-secret"')]:
            with self.subTest(old=old):
                with self.assertRaises(ValueError) as raised:
                    compare_configs(self.ON, disabled_config(self.ON).replace(old, new))
                self.assertNotIn('new-secret', str(raised.exception))
                self.assertNotIn('private', str(raised.exception))

    def test_reject_identical_or_already_disabled_pair(self):
        with self.assertRaises(ValueError):
            compare_configs(self.ON, self.ON)
        with self.assertRaises(ValueError):
            disabled_config(disabled_config(self.ON))


if __name__ == '__main__':
    unittest.main()
