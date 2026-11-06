#!/usr/bin/env python3
"""Fault every cJSON allocation in 14 production request builders. No device/network I/O.

Requires C/C++ compilers, the project ESP-IDF cJSON, and, for --validate-schema,
Python jsonschema plus rfc3339-validator. Schema validation also covers Word's
canonical wire fixtures; candidate-order.json is an ordering algorithm input.
--source-root can target an immutable older checkout with the same interfaces.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def validate_word_fixtures(source, schema, checker):
    from jsonschema.validators import validator_for
    fixtures = source/'contracts/word-study-v1/fixtures'
    definitions = {
        'session-request.json': 'createSessionRequest',
        'session-request-review.json': 'createSessionRequest',
        'session-request-intake.json': 'createSessionRequest',
        'session-request-sequential-cursor.json': 'createSessionRequest',
        'session-response.json': 'createSessionResponse',
        'candidate-page-request.json': 'candidatePageRequest',
        'candidate-page-response.json': 'candidatePageResponse',
        'observation-request.json': 'observationRequest',
        'observation-response.json': 'observationResponse',
        'skip-request.json': 'skipObservationRequest',
        'skip-response.json': 'skipObservationResponse',
        'manifest-request.json': 'manifestRequest',
        'manifest-response.json': 'manifestResponse',
        # Long-sequence local sessions (added by the word_study_long_sequence_v1
        # contract). Classify every new wire fixture explicitly here so the
        # inventory assertion below stays a real guard.
        'local-session-request.json': 'localSessionRequest',
        'local-session-shuffle-request.json': 'localSessionRequest',
        'eligibility-request.json': 'eligibilityRequest',
        'eligibility-response.json': 'eligibilityResponse',
        'local-session-response.json': 'localSessionResponse',
        'observation-batch-request.json': 'observationBatchRequest',
        'observation-batch-response.json': 'observationBatchResponse',
        'observation-batch-partial-response.json': 'observationBatchResponse',
    }
    actual = {p.name for p in (fixtures/'valid').glob('*.json')}
    if actual != set(definitions) | {'candidate-order.json'}:
        raise SystemExit('FAIL: Word fixture inventory changed; classify every wire fixture explicitly')
    cases = [(name, definition, True, fixtures/'valid'/name) for name, definition in definitions.items()]
    cases.append(('unsafe-sequence.json', 'observationRequest', False, fixtures/'invalid/unsafe-sequence.json'))
    root_validator = validator_for(schema)(schema, format_checker=checker)
    failures = 0
    for name, definition, expected, path in cases:
        selected = dict(schema, **{'$ref': f'#/$defs/{definition}'})
        selected.pop('oneOf', None)
        validator = validator_for(selected)(selected, format_checker=checker)
        payload = json.loads(path.read_text())
        for label, current in (('definition', validator), ('root', root_validator)):
            if current.is_valid(payload) != expected:
                failures += 1
                print(f'FAIL Word fixture {name} ({label}): expected valid={expected}', flush=True)
    print(f'Word canonical wire fixtures: {len(cases)} cases, {2*len(cases)} checks, {failures} failures', flush=True)
    return failures


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=root)
    parser.add_argument('--normal-json', type=Path, help='write successful JSON requests')
    parser.add_argument('--normal-only', action='store_true', help='skip injected failures')
    parser.add_argument('--validate-schema', action='store_true', help='also validate against pinned schemas')
    parser.add_argument('--cloud-root', type=Path, help='also verify mirrored Word JSON artifacts in this WQN worktree')
    args = parser.parse_args()
    source = args.source_root.resolve()
    if args.cloud_root:
        contract = source/'contracts/word-study-v1'
        other = args.cloud_root/'web/contracts/word-study-v1'
        paths = sorted(p.relative_to(contract) for p in contract.rglob('*.json'))
        if paths != sorted(p.relative_to(other) for p in other.rglob('*.json')):
            raise SystemExit('FAIL: mirrored Word JSON file inventory drift')
        for path in paths:
            if (contract/path).read_bytes() != (other/path).read_bytes():
                raise SystemExit('FAIL: mirrored Word artifact byte drift: '+str(path))
        print(f'{len(paths)} Word JSON artifacts are byte-identical across repositories', flush=True)
    cjson = Path(os.environ.get('IDF_PATH', '/home/unknow/esp/esp-idf-v5.5')) / 'components/json/cJSON'
    component = source / 'components/device_protocol'
    schemas = {}
    defines = []
    for name, macro in [('device-control-v3', 'DEVICE_CONTROL'), ('word-study-v1', 'WORD_STUDY'),
                        ('note-study-v1', 'NOTE_STUDY'), ('problem-study-v1', 'PROBLEM_STUDY')]:
        data = (source / f'contracts/{name}/{name}.schema.json').read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        manifest = json.loads((source / f'contracts/{name}/manifest.json').read_text())
        if digest != manifest['schema_sha256']:
            raise SystemExit(f'FAIL: {name} schema hash drift')
        schemas[name] = json.loads(data)
        defines.append(f'-DWQN_{macro}_SCHEMA_SHA256="{digest}"')
    sanitizer = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
    with tempfile.TemporaryDirectory(prefix='wqn-protocol-oom-') as directory:
        tmp = Path(directory)
        (tmp / 'esp_check.h').write_text(
            '#pragma once\n#include "esp_err.h"\n'
            '#define ESP_RETURN_ON_ERROR(expr, ...) do { const esp_err_t err_ = (expr); '
            'if (err_ != ESP_OK) return err_; } while (0)\n')
        obj = tmp / 'cJSON.o'
        subprocess.run(['gcc', '-std=c99', '-Wall', '-Wextra', '-Werror', *sanitizer,
                        '-I'+str(cjson), '-c', str(cjson / 'cJSON.c'), '-o', str(obj)], check=True, timeout=30)
        binary = tmp / 'test'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', *sanitizer,
                        '-I'+str(tmp), '-I'+str(root / 'scripts/testdata/sync_host'),
                        '-I'+str(component / 'include'), '-I'+str(cjson), *defines,
                        str(root / 'scripts/testdata/protocol_request_oom.cpp'),
                        *[str(component / f'{name}.cpp') for name in ('v3', 'word_study', 'note_study', 'problem_study')],
                        str(obj), '-o', str(binary)], check=True, timeout=60)
        normal = subprocess.run([str(binary), '--normal-json'], capture_output=True,
                                text=True, env=env, check=True, timeout=15).stdout
        rows = [json.loads(line) for line in normal.splitlines()]
        print(f'{len(rows)} normal requests serialized successfully as JSON', flush=True)
        if args.normal_json:
            args.normal_json.write_text(normal)
        if args.validate_schema:
            from jsonschema import FormatChecker
            from jsonschema.validators import validator_for
            checker = FormatChecker()
            if 'date-time' not in checker.checkers:
                raise SystemExit('FAIL: date-time checker unavailable; install rfc3339-validator')
            failed = 0
            for row in rows:
                schema = dict(schemas[row['schema']], **{'$ref': f"#/$defs/{row['definition']}"})
                schema.pop('oneOf', None)
                schema.pop('anyOf', None)
                errors = list(validator_for(schema)(schema, format_checker=checker).iter_errors(row['payload']))
                if errors:
                    failed += 1
                    print(f"FAIL schema {row['name']}: {errors[0].message}", flush=True)
            print(f'{len(rows) - failed}/{len(rows)} requests passed pinned JSON Schema validation', flush=True)
            if failed:
                return 1
            if validate_word_fixtures(source, schemas['word-study-v1'], checker):
                return 1
        if args.normal_only:
            return 0
        return subprocess.run([str(binary)], env=env, timeout=30).returncode


if __name__ == '__main__':
    raise SystemExit(main())
