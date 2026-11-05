#!/usr/bin/env python3
"""Host verification for long-sequence local-session / observation-batch codec.

Compiles the REAL production word_study.cpp (and v3.cpp), the REAL protocol
types (word_study.h / v3.h) and the REAL SDK cJSON under ASan/UBSan/LSan. ESP
ports are stubbed only. No fake structs stand in for the production codec or
its allocator/API shape.

The C++ fixture exercises builders, fail-closed behaviour, response parsing
and the standalone receipt validator. This runner additionally:
  * validates the two built requests against the pinned word-study-v1 JSON
    Schema (with a local RFC3339 date-time checker),
  * value-compares them to the golden request fixtures,
  * validates the golden response / partial-response fixtures against their
    schema definitions.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    fixtures = root / 'contracts/word-study-v1/fixtures/valid'
    schema_path = root / 'contracts/word-study-v1/word-study-v1.schema.json'
    schema_bytes = schema_path.read_bytes()
    digest = hashlib.sha256(schema_bytes).hexdigest()
    manifest = json.loads((root / 'contracts/word-study-v1/manifest.json').read_text())
    if digest != manifest['schema_sha256']:
        raise SystemExit('FAIL: word-study-v1 schema hash drift')
    schema = json.loads(schema_bytes)

    cjson = Path(os.environ.get('IDF_PATH', '/home/unknow/esp/esp-idf-v5.5')) / 'components/json/cJSON'
    component = root / 'components/device_protocol'

    with tempfile.TemporaryDirectory(prefix='wqn-longseq-codec-') as directory:
        tmp = Path(directory)
        (tmp / 'esp_err.h').write_text(
            '#pragma once\nusing esp_err_t = int;\n'
            'constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=0x101,\n'
            '    ESP_ERR_INVALID_ARG=0x102, ESP_ERR_INVALID_STATE=0x103,\n'
            '    ESP_ERR_NOT_FOUND=0x105, ESP_ERR_NOT_SUPPORTED=0x106, ESP_ERR_TIMEOUT=0x107,\n'
            '    ESP_ERR_INVALID_RESPONSE=0x108, ESP_ERR_INVALID_SIZE=0x109;\n')
        (tmp / 'esp_check.h').write_text(
            '#pragma once\n#include "esp_err.h"\n'
            '#define ESP_RETURN_ON_ERROR(expr, ...) do { const esp_err_t err_ = (expr); '
            'if (err_ != ESP_OK) return err_; } while (0)\n')
        defines = [
            f'-DWQN_WORD_STUDY_SCHEMA_SHA256="{digest}"',
            f'-DWQN_DEVICE_CONTROL_SCHEMA_SHA256="{digest}"',
        ]
        sanitizer = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                     '-fno-omit-frame-pointer']
        binary = tmp / 'test'
        subprocess.run([
            'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', *sanitizer,
            '-I' + str(tmp), '-I' + str(root / 'scripts/testdata/sync_host'),
            '-I' + str(component / 'include'), '-I' + str(cjson), *defines,
            str(root / 'scripts/testdata/word_longseq_codec.cpp'),
            str(component / 'word_study.cpp'),
            str(component / 'v3.cpp'),
            str(cjson / 'cJSON.c'), '-o', str(binary),
        ], check=True, timeout=120)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',
                   UBSAN_OPTIONS='halt_on_error=1')
        proc = subprocess.run([str(binary)], capture_output=True, text=True,
                              env=env, timeout=60)
        if proc.returncode != 0:
            sys.stderr.write(proc.stdout)
            sys.stderr.write(proc.stderr)
            raise SystemExit(f'FAIL: codec binary exited {proc.returncode}')

        built = {}
        cases = []
        summary = None
        for line in proc.stdout.splitlines():
            if line.startswith('BUILD_REGISTER '):
                built['register'] = line[len('BUILD_REGISTER '):]
            elif line.startswith('BUILD_BATCH '):
                built['batch'] = line[len('BUILD_BATCH '):]
            elif line.startswith('CASE '):
                _, name, verdict = line.split(' ', 2)
                cases.append((name, verdict))
            elif line.startswith('SUMMARY '):
                _, total, failed = line.split(' ')
                summary = (int(total), int(failed))

        if summary is None or summary[1] != 0:
            raise SystemExit(f'FAIL: C++ checks had failures: {summary}')
        cpp_fail = sum(1 for _, v in cases if v != 'PASS')
        print(f'{len(cases)} C++ codec checks, {cpp_fail} failures')

        # ---- Schema + golden validation of the built requests ----
        from jsonschema import FormatChecker
        from jsonschema.validators import validator_for

        @FormatChecker.cls_checks('date-time', raises=())
        def _date_time(value):
            return bool(isinstance(value, str) and re.match(
                r'^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?(Z|[+-]\d{2}:\d{2})$',
                value))

        checker = FormatChecker()
        if 'date-time' not in checker.checkers:
            raise SystemExit('FAIL: date-time format checker unavailable')

        def validate(payload, definition):
            selected = dict(schema, **{'$ref': f'#/$defs/{definition}'})
            selected.pop('oneOf', None)
            selected.pop('anyOf', None)
            errors = list(validator_for(selected)(selected, format_checker=checker)
                          .iter_errors(payload))
            if errors:
                raise SystemExit(f'FAIL schema {definition}: {errors[0].message}')

        reg_payload = json.loads(built['register'])
        batch_payload = json.loads(built['batch'])
        validate(reg_payload, 'localSessionRequest')
        validate(batch_payload, 'observationBatchRequest')
        print('2 built requests pass pinned JSON Schema validation')

        golden_reg = json.loads((fixtures / 'local-session-request.json').read_text())
        golden_batch = json.loads((fixtures / 'observation-batch-request.json').read_text())
        if reg_payload != golden_reg:
            raise SystemExit('FAIL: built registration request != golden fixture')
        if batch_payload != golden_batch:
            raise SystemExit('FAIL: built batch request != golden fixture')
        print('2 built requests are value-identical to the golden fixtures')

        # ---- Validate the golden response fixtures against the schema ----
        responses = {
            'local-session-response.json': 'localSessionResponse',
            'observation-batch-response.json': 'observationBatchResponse',
            'observation-batch-partial-response.json': 'observationBatchResponse',
        }
        for name, definition in responses.items():
            validate(json.loads((fixtures / name).read_text()), definition)
        print(f'{len(responses)} golden response fixtures pass schema validation')

        # ---- Confirm the server-fixed semantics are absent from the request ----
        fixed = ('domain', 'mode', 'purpose', 'ordering', 'candidate_policy_version')
        assert not any(k in reg_payload for k in fixed), 'server-fixed fields leaked'
        assert not any(k in batch_payload for k in fixed), 'server-fixed fields leaked'
        print('server-fixed semantics fields are absent from both requests')
        print('ALL LONG-SEQUENCE CODEC HOST CHECKS PASSED')
        return 0


if __name__ == '__main__':
    raise SystemExit(main())
