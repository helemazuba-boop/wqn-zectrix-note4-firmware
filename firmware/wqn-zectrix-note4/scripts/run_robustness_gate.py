#!/usr/bin/env python3
"""Fault-model regression inventory used locally and by firmware CI. NOT HIL.

Negative controls must finish their assertions and reject the intended semantic
fault. Compiler errors, timeouts, sanitizer errors and Python exceptions fail
the gate; a nonzero exit alone never counts as a detected mutation.
"""
import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re
import subprocess
import sys
import time


@dataclass(frozen=True)
class Case:
    name: str
    model: str
    group: str
    args: tuple
    rejection: str = ''
    completion: str = ''


def cases():
    result = []
    for name, model, args in (
        ('word_offline_dispatch', 'Offline words: cached controls do not acquire radio demand', ()),
        ('protocol_request_oom', 'R-28/N-03/N-05: allocation failure and pinned wire contracts', ('--validate-schema',)),
        ('note_install_serialization', 'R-01: full/targeted installation interleavings', ()),
        ('manifest_load_preservation', 'R-06: I/O or memory failure preserves caches', ()),
        ('agent_voice_finalize', 'R-22: matching transport terminal during finalize', ()),
        ('stream_transport', 'R-12/R-23: bounded HTTP/control/SSE and interrupted reads', ('--validate-pages',)),
        ('flash_frame_assembly', 'R-29: rejected oversized frames cannot resume through later fragments', ()),
        ('agent_run_progress', 'R-30: submitted run stalls without first event', ()),
        ('pcm_preroll', 'R-26: tap/start commit order and rejected queues', ()),
        ('ai_force_full', 'R-32: full-refresh intent survives dispatch/defer/reject', ()),
        ('v3_sync_response', 'R-31: UUID/maxItems and transactional output', ()),
        ('problem_request_contract', 'N-01: strict complete Problem requests', ()),
        ('json_string_boundaries', 'N-04/N-06: external NUL and Unicode identity', ()),
        ('pack_json_boundaries', 'N-06: complete persisted pack text', ()),
        ('note_index_delivery', 'N-02: delayed results across full/targeted lanes', ()),
        ('storage_schema', 'R-18: explicit recovery intent, stale consent and boot replay', ()),
        ('storage_mount_guard', 'R-18: ambiguous SPIFFS registration errors require actual mount proof', ()),
        ('outbox_forensics', 'R-08 ongoing: offline raw preservation and native journal/SDK CRC parity', ()),
        ('outbox_reconcile', 'R-08 ongoing: pinned raw evidence, authenticated receipts and non-authorizing review plans', ()),
        ('outbox_checkpoint', 'R-08 ongoing: native codec parity and local candidate checkpoint validation', ()),
        ('outbox_candidate', 'R-08 ongoing: native combined replay, append-only candidates and independent verification', ()),
        ('storage_maintenance', 'R-08 ongoing: owner-only raw NVS evidence and no stale cursor fallback', ()),
        ('storage_evidence_capture', 'R-08 ongoing: frozen owner raw files/NVS, pinned stream receive and independent replay', ()),
        ('raw_flash_evidence', 'R-08 ongoing: unmounted main Flash, raw bytes, full-range recheck and independent replay', ()),
        ('word_cursor_guards', 'R-08 ongoing: pause source validation, migration and explicit read errors', ()),
        ('word_deck_scope_tail', 'R-08/R-18: preserve unknown scope metadata and replay only valid interrupted changes', ()),
        ('word_scope_autorepair', 'R-08: storage readiness and accepted manual/auto scope obligations', ()),
        ('word_local_descriptor', 'R-08: full native session/descriptor file preservation when scope is unresolved', ()),
        ('word_session_replay', 'R-08 ongoing: checkpoint replay and terminal cursor read failures', ()),
        ('pack_prune', 'R-08: unknown session pins preserve current and rollback packs', ()),
        ('sync_journal', 'N-06: opaque checkpoints and decode allocation faults', ()),
        ('sync_control', 'R-21: raw SEQUENCE_GAP, retry checkpoint and restart escalation', ()),
        ('sync_retry', 'R-21: bounded retries and independent outbox identities', ()),
        ('word_ack_upload', 'R-21: ACK batches, failures, yields and quiet maintenance', ()),
        ('agent_disabled', 'R-21: disabled Agent entry points refuse work', ()),
        ('display_recovery', 'R-10: cross-intent failure budget and recovery at 100/1000 Hz', ()),
        ('display_result_capacity', 'R-04: reserved queue capacity and receive-before-ACK at 100/1000 Hz', ()),
        ('ws_event_lifetime', 'R-25: client generations, pending callbacks and legal SDK stop ordering', ()),
    ):
        result.append(Case(name, model, 'runtime', (f'test_{name}.py', *args)))
    for name, model in (
        ('ws_release_deadline', 'R-24: missing release and stale request/generation'),
        ('storage_prepare_deadline', 'R-07: absolute deadline and detached owner lifetime'),
        ('storage_recovery', 'R-18: recovery model, owner delivery and UI confirmation'),
    ):
        for hz in (100, 1000):
            result.append(Case(f'{name}-{hz}', model, 'runtime',
                               (f'test_{name}.py', '--tick-hz', str(hz))))
    for name in ('word', 'note', 'problem', 'home', 'todo', 'settings', 'navigation', 'agent', 'display_submit', 'stack_gate'):
        result.append(Case(f'ui-{name}', f'R-21: UI {name} production behavior/gate fixtures', 'ui', (f'test_ui_{name}.py',)))
    result.append(Case('mount-guard-forget-mount-proof', 'R-18: an invalid-state error alone cannot prove storage is mounted', 'controls',
                       ('test_storage_mount_guard.py', '--mutation', 'forget-mount-proof'), r'^FAIL mount-guard:',
                       r'Storage mount guard: \d+ checks, [1-9]\d* failures \(production function; host SPIFFS API; NOT HIL\)'))
    for mutation in ('no-gap-escalation', 'forget-gap-session', 'drop-restored-attempt', 'retire-other-session'):
        result.append(Case(f'sync-{mutation}', 'R-21: escalation, restored attempts and session isolation', 'controls',
                           ('test_sync_control.py', '--mutation', mutation), r'^FAIL: GAP:', r'\d+ PASS / [1-9]\d* FAIL \(production control paths; NOT HIL\)'))
    for mutation in ('bypass-admission', 'early-ack', 'duplicate-terminal', 'short-queue'):
        result.append(Case(f'display-capacity-{mutation}', 'R-04: reservation/terminal/ACK proof assumptions', 'controls',
                           ('test_display_result_capacity.py', '--mutation', mutation), r'^FAIL capacity:',
                           r'Display result capacity: \d+ checks, [1-9]\d* failures'))
    for mutation in ('keep-backlog', 'ignore-client', 'ignore-generation', 'trust-raw-bits'):
        result.append(Case(f'ws-lifetime-{mutation}', 'R-25: retired payload, client identity, generation and wake-only semantics', 'controls',
                           ('test_ws_event_lifetime.py', '--mutation', mutation), r'^FAIL ',
                           r'WS event lifetime: \d+ checks, [1-9]\d* failures'))
    for mutation in ('short-hold', 'unpresented-reset', 'forget-uncertain'):
        result.append(Case(f'recovery-{mutation}', 'R-18: fresh held confirmation, presentation and uncertain writes', 'controls',
                           ('test_storage_recovery.py', '--part', 'model', '--mutation', mutation),
                           r'^FAIL recovery-model:', r'Storage recovery model: \d+ checks, [1-9]\d* failures'))
    for mutation in ('wrong-crc', 'trust-analysis', 'legacy-word-capacity', 'ignore-descriptors'):
        result.append(Case(f'forensics-{mutation}', 'R-08 ongoing: CRC, profile capacity, descriptors and independently recomputed reports', 'controls',
                           ('test_outbox_forensics.py', '--mutation', mutation), r'^FAIL:',
                           r'Outbox forensics: \d+ scenarios, [1-9]\d* failures, 0 errors \(NOT HIL\)'))
    for mutation in ('trust-request-id', 'forget-suspension', 'trust-assessment'):
        result.append(Case(f'reconcile-{mutation}', 'R-08 ongoing: exact payload, suspend intent and derived evidence', 'controls',
                           ('test_outbox_reconcile.py', '--mutation', mutation), r'^FAIL reconcile:',
                           r'Outbox reconciliation: \d+ scenarios, [1-9]\d* failures, 0 errors \(NOT HIL\)'))
    for mutation in ('allow-window-mismatch', 'trust-legacy-nvs'):
        result.append(Case(f'checkpoint-{mutation}', 'R-08 ongoing: representable checkpoint and unverified NVS context', 'controls',
                           ('test_outbox_checkpoint.py', '--mutation', mutation), r'^FAIL checkpoint:',
                           r'Outbox checkpoint: \d+ scenarios, [1-9]\d* failures, 0 errors \(NOT HIL\)'))
    for mutation in ('trust-candidate', 'normalize-unknown-session'):
        result.append(Case(f'native-candidate-{mutation}', 'R-08 ongoing: independent native recomputation and exact retained bytes', 'controls',
                           ('test_outbox_candidate.py', '--mutation', mutation), r'^FAIL native-candidate:',
                           r'Native outbox candidate: \d+ scenarios, [1-9]\d* failures, 0 errors \(NOT HIL\)'))
    result.append(Case('word_offline_dispatch-force-network', 'Offline words: reject network demand for cached controls', 'controls',
                       ('test_word_offline_dispatch.py', '--mutation', 'force-network'), r'^FAIL:',
                       r'Offline word dispatch: \d+ checks, [1-9]\d* failures \(NOT HIL\)'))
    result.append(Case('agent-wire-unbounded-budget', 'Agent: wire health remains bounded while business progress waits', 'controls',
                       ('test_agent_run_progress.py', '--mutation', 'wire-unbounded-budget'), r'^FAIL ',
                       r'Agent stream/client: \d+ checks, [1-9]\d* failures;'))
    for mutation in ('ignore-consent', 'ignore-size', 'skip-recheck'):
        result.append(Case(f'raw-flash-{mutation}', 'R-08: sensitive scope, physical size and source reread cannot be bypassed', 'controls',
                           ('test_raw_flash_evidence.py', '--mutation', mutation), r'^FAIL raw-flash:',
                           r'Raw Flash evidence: \d+ scenarios, [1-9]\d* failures, 0 errors \(host SDK ports; NOT HIL\)'))
    for mutation in ('ignore-nvs-size', 'truncate-size', 'write-open'):
        result.append(Case(f'maintenance-nvs-{mutation}', 'R-08: raw size, explicit presence and read-only evidence', 'controls',
                           ('test_storage_maintenance.py', '--mutation', mutation), r'^FAIL maintenance-nvs:',
                           r'Storage maintenance NVS: \d+ checks, [1-9]\d* failures \(host ports; NOT HIL\)'))
    for mutation in ('ignore-mode', 'skip-recheck', 'write-open', 'trust-transcript', 'omit-eligibility'):
        result.append(Case(f'evidence-capture-{mutation}', 'R-08: maintenance admission, raw source recheck, read-only NVS and transcript integrity', 'controls',
                           ('test_storage_evidence_capture.py', '--mutation', mutation), r'^FAIL evidence:',
                           r'Storage evidence: \d+ scenarios, [1-9]\d* failures, 0 errors \(host ports; NOT HIL\)'))
    result.append(Case('word-cursor-swallow-error', 'R-08: failed cursor cannot restore an unpaused session', 'controls',
                       ('test_word_session_replay.py', '--mutation', 'swallow-cursor-error'), r'^FAIL: cursor read failure',
                       r'\d+ PASS / [1-9]\d* FAIL \(host storage mocks; NOT power-loss or concurrency HIL\)'))
    for mutation in ('truncate-generation', 'erase-invalid-marker', 'accept-invalid-uuid', 'publish-unverified-cache',
                     'skip-prior-tail', 'reopen-after-failure'):
        result.append(Case(f'deck-scope-{mutation}', 'R-08/R-18: unknown scope evidence cannot authorize destructive replay', 'controls',
                           ('test_word_deck_scope_tail.py', '--mutation', mutation), r'^FAIL scope-recovery:',
                           r'Deck scope recovery: \d+ checks, [1-9]\d* failures \(host NVS ports; NOT HIL\)'))
    result.append(Case('word-ignore-scope-fence', 'R-08: unresolved scope cannot authorize journal/session/GC changes', 'controls',
                       ('test_word_session_replay.py', '--mutation', 'ignore-scope-fence'), r'^FAIL: unready scope',
                       r'\d+ PASS / [1-9]\d* FAIL \(host storage mocks; NOT power-loss or concurrency HIL\)'))
    for name, mutation, summary in (
        ('display_recovery', 'no-limit', 'Display recovery'),
        ('storage_prepare_deadline', 'double-budget', 'Storage preparation deadline'),
        ('ai_force_full', 'drop-update-force', 'AI force-full dispatch'),
        ('v3_sync_response', 'skip-uuid', 'V3 sync response'),
        ('note_index_delivery', 'accept-old-bulk', 'Note index delivery'),
    ):
        result.append(Case(f'{name}-{mutation}', f'{name}: reject removed semantic guard', 'controls',
                           (f'test_{name}.py', '--mutation', mutation), r'^FAIL ',
                           re.escape(summary) + r': \d+ checks, [1-9]\d* failures'))
    result.append(Case('json_string_boundaries-bad-hex', 'N-04/N-06: malformed Unicode guard', 'controls',
                       ('test_json_string_boundaries.py', '--mutation', 'bad-hex'), r'^FAIL ',
                       r'JSON string boundaries: \d+ protocol cases, \d+ checks, [1-9]\d* failures'))
    return result


INFRA_FAILURE = re.compile(r'Traceback \(most recent call last\)|AddressSanitizer|LeakSanitizer|runtime error:|fatal error:|\berror:', re.I)


def accepted(case, returncode, output):
    if INFRA_FAILURE.search(output):
        return False
    if not case.rejection:
        return returncode == 0
    return (returncode == 1 and re.search(case.rejection, output, re.M) is not None and
            re.search(case.completion, output, re.M) is not None)


def selftest():
    control = Case('control', 'fixture', 'controls', (), r'^FAIL assertion', r'2 checks, 1 failures')
    good = 'FAIL assertion\n2 checks, 1 failures\n'
    for code, output, expected in (
        (1, good, True), (0, good, False), (1, 'FAIL: mutation boundary drift', False),
        (1, 'FAIL assertion\nTraceback (most recent call last)\n2 checks, 1 failures', False),
        (1, good + 'fatal error: missing header', False), (1, good + 'runtime error: UB', False),
        (-11, good, False), (2, good, False), (1, '2 checks, 1 failures', False),
        (1, good + 'ERROR: AddressSanitizer: failure', False),
    ):
        if accepted(control, code, output) != expected:
            raise SystemExit('FAIL: negative-control result classification')
    print('PASS: 10 runner failure-classification cases (NOT HIL)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--group', choices=('runtime', 'ui', 'controls', 'all'), default='all')
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--selftest', action='store_true')
    args = parser.parse_args()
    if args.selftest:
        selftest()
        return 0
    selected = [c for c in cases() if args.group == 'all' or c.group == args.group]
    if args.list:
        print(json.dumps([dict(name=c.name, fault_model=c.model, group=c.group, command=list(c.args))
                          for c in selected], indent=2))
        return 0
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parent
    results = []
    for case in selected:
        started = time.monotonic()
        try:
            done = subprocess.run([sys.executable, str(root / case.args[0]), *case.args[1:]],
                                  cwd=root.parent, text=True, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, timeout=180, check=False)
            output = done.stdout
            passed = accepted(case, done.returncode, output)
            code = done.returncode
        except (OSError, subprocess.TimeoutExpired):
            output, passed, code = 'FAIL: runner could not complete the child process\n', False, None
        results.append(dict(name=case.name, fault_model=case.model, passed=passed,
                            returncode=code, seconds=round(time.monotonic() - started, 3)))
        if args.output_dir:
            (args.output_dir / f'{case.name}.log').write_text(output, encoding='utf-8')
            (args.output_dir / 'results.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
        print(f'{"PASS" if passed else "FAIL"}: {case.name} ({results[-1]["seconds"]}s)', flush=True)
        if not passed:
            print(output, flush=True)
            return 1
    print(f'{len(results)}/{len(selected)} robustness gate cases passed (host only; NOT HIL)', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
