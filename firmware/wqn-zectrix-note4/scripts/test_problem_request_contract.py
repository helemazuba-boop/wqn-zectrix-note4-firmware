#!/usr/bin/env python3
"""Validate canonical Problem request cases with Draft2020-12 and format checks.

No SDK/device/network calls. --before selects only the schema/manifest revision;
the same current request cases and unchanged wire fixtures exercise both.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess

from jsonschema import FormatChecker
from jsonschema.validators import validator_for


def main():
    root = Path(__file__).resolve().parents[1]
    contract = root/'contracts/problem-study-v1'
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before')
    parser.add_argument('--cloud-root', type=Path, help='WQN repository root for byte identity checks')
    parser.add_argument('--mutation', choices=('closed-allof', 'open-manifest', 'open-observation',
                        'missing-metadata', 'missing-cursor', 'unconstrained-metadata'))
    args = parser.parse_args()

    def read(name):
        if args.before:
            return subprocess.check_output(['git', 'show',
                f'{args.before}:firmware/wqn-zectrix-note4/contracts/problem-study-v1/{name}'], cwd=root)
        return (contract/name).read_bytes()

    data = read('problem-study-v1.schema.json')
    manifest = json.loads(read('manifest.json'))
    assert hashlib.sha256(data).hexdigest() == manifest['schema_sha256'], 'schema/manifest hash mismatch'
    schema = json.loads(data)
    validator_for(schema).check_schema(schema)
    if args.cloud_root:
        other = args.cloud_root/'web/contracts/problem-study-v1'
        # JSON wire artifacts are mirrored. Cloud-only explanatory Markdown
        # (README/server cache links) is not a firmware contract artifact.
        paths = sorted(p.relative_to(contract) for p in contract.rglob('*.json'))
        assert paths == sorted(p.relative_to(other) for p in other.rglob('*.json')), 'JSON contract file set drift'
        for path in paths:
            assert (contract/path).read_bytes() == (other/path).read_bytes(), str(path)+' byte drift'
        print(f'{len(paths)} Problem JSON contract files are byte-identical across both repositories', flush=True)

    if args.mutation == 'closed-allof':
        for name in ('manifestRequest', 'observationRequest'):
            schema['$defs'][name]['allOf'] = [{'$ref': '#/$defs/requestMetadata'}]
    elif args.mutation in ('open-manifest', 'open-observation'):
        name = 'manifestRequest' if args.mutation == 'open-manifest' else 'observationRequest'
        schema['$defs'][name]['additionalProperties'] = True
    elif args.mutation == 'missing-metadata':
        for name in ('manifestRequest', 'observationRequest'):
            schema['$defs'][name]['required'].remove('boot_id')
    elif args.mutation == 'missing-cursor':
        schema['$defs']['manifestRequest']['required'].remove('cursor')
    elif args.mutation == 'unconstrained-metadata':
        schema['$defs']['requestMetadata']['properties']['firmware_version'] = {}
    checker = FormatChecker()
    if 'date-time' not in checker.checkers:
        raise SystemExit('FAIL: date-time checker unavailable; install rfc3339-validator in the test environment')
    root_validator = validator_for(schema)(schema, format_checker=checker)
    cases = json.loads((contract/'fixtures/request-validation-cases.json').read_text())['cases']
    checks = failures = 0
    for row in cases:
        payload = json.loads((contract/'fixtures/valid'/row['fixture']).read_text())
        for key in row.get('remove', []):
            del payload[key]
        payload.update(row.get('set', {}))
        if 'replace' in row:
            payload = row['replace']
        definition = dict(copy.deepcopy(schema), **{'$ref': '#/$defs/'+row['definition']})
        definition.pop('oneOf', None)
        validator = validator_for(definition)(definition, format_checker=checker)
        valid = validator.is_valid(payload)
        checks += 1
        if valid != row['expected']:
            failures += 1
            print(f"FAIL {row['name']}: expected {row['expected']}, got {valid}", flush=True)
        if row['definition'] != 'requestMetadata':
            checks += 1
            if root_validator.is_valid(payload) != row['expected']:
                failures += 1
                print(f"FAIL root union {row['name']}", flush=True)
    print(f'Problem request schema: {len(cases)} cases, {checks} checks, {failures} failures (Python Draft2020-12; NOT HIL)', flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
