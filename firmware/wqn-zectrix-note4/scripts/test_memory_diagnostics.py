#!/usr/bin/env python3
"""Exercise production RAM recorder, bounded trace, and serial-log summarizer."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from analyze_memory_log import parse_log, summarize


def sample(seq, stage='periodic', operation=0, free=10000):
    fields = f'seq={seq} t_ms={seq * 1000} stage={stage} op={operation}'
    for pool in ('internal', 'dma', 'psram'):
        fields += f' {pool}_free={free} {pool}_min=5000 {pool}_largest=4000'
    return f'I (123) mem_diag: sample {fields} failures=2 sample_lost=1 failure_lost=0\n'


def history_sample(seq, export=1):
    return sample(seq).replace('mem_diag: sample', 'mem_diag: history_sample').rstrip() + f' export={export}\n'


class SerialTests(unittest.TestCase):
    def test_history_with_interleaved_live_records(self):
        records = [sample(900), 'mem_diag: history_begin export=1 samples=2 events=0 failures=1 '
                   'sample_overwritten=4 event_overwritten=7 failure_overwritten=0\n',
                   history_sample(10), sample(901), history_sample(30),
                   'mem_diag: history_alloc_failed seq=1 t_ms=2000 bytes=221 caps=0x80c '
                   'function=heap_caps_malloc isr=1 failure_lost=0 export=1\n',
                   'mem_diag: history_end export=1 emitted=3 overwritten_during_export=0\n', sample(902)]
        samples, failures, history = parse_log(records, with_metadata=True)
        self.assertEqual([r['seq'] for r in samples], [10, 30])
        self.assertEqual(failures[0]['bytes'], 221)
        report = summarize(samples, failures, history)
        self.assertIn('complete=yes', report)
        self.assertIn('samples=4 events=7 failures=0', report)
        live, _ = parse_log(records, prefer_history=False)
        self.assertEqual([r['seq'] for r in live], [900, 901, 902])

    def test_history_selects_latest_export_and_reports_incomplete(self):
        records = ['mem_diag: history_begin export=1 samples=1 events=0 failures=0\n',
                   history_sample(1), 'mem_diag: history_end export=1 emitted=1 overwritten_during_export=0\n',
                   'mem_diag: history_begin export=2 samples=2 events=0 failures=0\n', history_sample(20, 2)]
        samples, failures, history = parse_log(records, with_metadata=True)
        self.assertEqual([r['seq'] for r in samples], [20])
        self.assertIn('history_export=2 complete=no', summarize(samples, failures, history))
        self.assertIn('Incomplete history export', summarize(samples, failures, history))

    def test_history_missing_line_or_overwritten_during_export(self):
        for ending in ('emitted=2 overwritten_during_export=0',
                       'emitted=1 overwritten_during_export=1'):
            records = ['mem_diag: history_begin export=1 samples=2 events=0 failures=0\n',
                       history_sample(1), f'mem_diag: history_end export=1 {ending}\n']
            samples, failures, history = parse_log(records, with_metadata=True)
            self.assertIn('complete=no', summarize(samples, failures, history))
        samples, failures, history = parse_log([history_sample(1),
            'mem_diag: history_end export=1 emitted=1 overwritten_during_export=0\n'], with_metadata=True)
        self.assertIn('complete=no', summarize(samples, failures, history))

    def test_boot_and_lost_records(self):
        samples, failures = parse_log([sample(1, 'boot'), sample(7), sample(1, 'boot')])
        self.assertEqual([r['session'] for r in samples], [0, 0, 1])
        self.assertFalse(failures)
        report = summarize(samples, failures)
        self.assertIn('session=0 samples=2', report)
        self.assertIn('lost: samples=1 failures=0; failed_allocations=2', report)

    def test_concurrent_operation_pairing(self):
        records = [sample(1, 'ai_begin', 1, 15000), sample(2, 'ai_begin', 2, 14000),
                   sample(3, 'ai_end', 1, 12000), sample(4, 'ai_end', 2, 14500),
                   sample(5, 'ai_end', 3)]
        samples, failures = parse_log(records)
        self.assertIn('paired_operations=2 internal_free_delta_range=-3000..+500 B',
                      summarize(samples, failures))

    def test_truncation_ansi_and_failure_caps(self):
        records = ['unrelated wifi log\n', '\x1b[0;32m' + sample(1) + '\x1b[0m',
                   'mem_diag: sample seq=2 t_ms=2000 stage=periodic\n',
                   'mem_diag: sample seq=bad t_ms=bad stage=periodic\n',
                   'W (2) mem_diag: alloc_failed seq=1 t_ms=999 bytes=221 '
                   'caps=0x0000080c function=heap_caps_malloc isr=1 failure_lost=0\n']
        samples, failures = parse_log(records)
        self.assertEqual(len(samples), 1)
        self.assertEqual(failures[0]['caps'], 0x80c)
        self.assertIn('bytes=221 caps=0x0000080c', summarize(samples, failures))

    def test_cli_csv(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix='note4-memory-csv-') as temporary:
            log, csv = Path(temporary)/'serial.log', Path(temporary)/'memory.csv'
            log.write_text(sample(1, 'boot'), encoding='utf-8-sig')
            subprocess.run(['python3', str(root/'scripts/analyze_memory_log.py'),
                            str(log), '--csv', str(csv)], check=True,
                           stdout=subprocess.PIPE, text=True, timeout=10)
            self.assertEqual(len(csv.read_text().splitlines()), 2)
            self.assertIn('internal_largest', csv.read_text())
        self.assertIn('No complete', summarize([], []))


def run_production_tests():
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='note4-memory-host-') as temporary:
        port = Path(temporary)
        for name in ('esp_attr.h', 'esp_heap_caps.h', 'esp_log.h', 'esp_timer.h',
                     'esp_heap_trace.h', 'esp_memory_utils.h', 'freertos/FreeRTOS.h',
                     'freertos/task.h'):
            header = port/name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#pragma once\n#include "memory_diagnostics_ports.h"\n')
        for enabled, tracing, history in ((1, 0, 0), (1, 0, 1), (1, 1, 1), (0, 0, 0)):
            (port/'sdkconfig.h').write_text(f'''#pragma once
#define CONFIG_WQN_MEMORY_DIAGNOSTICS {enabled}
#define CONFIG_WQN_MEMORY_TRACE_ENABLE {tracing}
#define CONFIG_WQN_MEMORY_HISTORY_ENABLE {history}
#define CONFIG_WQN_MEMORY_HISTORY_HOURS 24
#define CONFIG_WQN_MEMORY_SAMPLE_SECONDS 30
#define CONFIG_WQN_MEMORY_TRACE_DELAY_SECONDS 2
#define CONFIG_WQN_MEMORY_TRACE_SECONDS 3
#define CONFIG_WQN_MEMORY_TRACE_RECORDS 16
#define CONFIG_HEAP_TRACING_STACK_DEPTH 2
#define CONFIG_SPIRAM 1
#define CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM 1
''')
            binary = port/f'memory-{enabled}-{tracing}-{history}'
            subprocess.run(['g++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-pthread',
                            '-I', str(port), '-I', str(root/'scripts/testdata'),
                            '-I', str(root/'main'),
                            str(root/'scripts/testdata/memory_diagnostics.cpp'),
                            '-o', str(binary)], check=True, timeout=60)
            exported = port/f'history-{enabled}-{tracing}-{history}.log'
            subprocess.run([str(binary), str(exported)], check=True, timeout=15)
            if history:
                with exported.open() as stream:
                    samples, failures, metadata = parse_log(stream, with_metadata=True)
                assert len(samples) == 24 * 60 + 128 and len(failures) == 32
                assert 'complete=yes' in summarize(samples, failures, metadata)
                assert metadata['begin']['sample_overwritten'] == 3
                assert metadata['begin']['event_overwritten'] == 7
                assert metadata['begin']['failure_overwritten'] == 5
                assert failures[0]['failure_lost'] == 4


if __name__ == '__main__':
    run_production_tests()
    unittest.main()
