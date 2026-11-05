# N-03/N-05: Word golden fixtures and the root wire union

Firmware baseline `7383e1c`; cloud baseline `c139a44`. N-03 was confirmed in
the frozen follow-up evidence: the firmware's valid-labelled manifest response
used `compression: none`, contradicting its pinned `zlib` Schema. The cloud
golden fixture was already correct. Its manifest generator advertises zlib,
and the download route actually deflates the stored plain JSONL body.

The firmware golden manifest now matches the cloud copy. The skip-response
golden fixture's different example `server_time_ms` is also aligned; its data
payload was already identical. All 17 Word JSON contract artifacts now match
byte for byte. Stored pack metadata remains plain/none; the firmware parser's
existing legacy none compatibility is unchanged. Transport coding, stored
content and test fixtures are distinct boundaries.

Extending validation to every Word wire fixture exposed N-05: the pathless root
Schema used `oneOf` for overlapping endpoint request shapes. Candidate-page and
manifest requests have the same body shape; a full observation can also match
the compatibility skip request. Three normal fixtures therefore failed the
root despite passing their own endpoint definitions. The exact matched branches
and source commits are frozen in `n03/root-overlap-before.json`.

The five request variants are now grouped under one `anyOf` alternative inside
the existing root `oneOf`. The four response variants remain separate exclusive
alternatives. All `$defs`, including endpoint validation, remain unchanged.
The root catalogs allowed wire bodies; callers needing route-specific validation
must select that endpoint's definition. This does not change runtime Zod route
validation or add a field to distinguish requests that already share a shape.

The Word Schema digest changes from
`3bfc6f455fe0f5a6aceabc348a38ea9297919e7190ee81739a143977a69bc663`
to `e8e3603e0ed378ce2f30003c3ead5b65719aaa56362e74d77015d8508bf1f501`.
Both schema/manifest copies and the cloud digest constant are updated.
Long-sequence v1 later unifies both copies at
`2f6482f890485258fae8bf921f4e10da3733cd820342c9dd04535e1fc649f4d1`; see the
long-sequence section in `contracts/word-study-v1/README.md`. Word's
Schema and manifest are now configure dependencies, so incremental builds rerun
the existing hash gate when those files change. The generated Ninja configure
edge includes both files and its compile definitions contain the new digest.

The existing protocol-allocation test's `--validate-schema` mode now validates
13 valid Word wire fixtures and the invalid unsafe-sequence fixture against
both their selected definition and the root: 14 cases, 28 checks. It explicitly
classifies the non-wire `candidate-order.json` algorithm fixture and refuses
an unclassified new valid JSON fixture. Optional `--cloud-root` checks the whole
17-file Word JSON mirror. Schema mode also refuses an unavailable date-time
checker, using the isolated environment documented in N-01.

From the firmware application directory:

```sh
/tmp/wqn-robustness-schema-venv/bin/python scripts/test_protocol_request_oom.py \
  --validate-schema \
  --cloud-root /home/unknow/projects/.worktrees/WQN-robustness-20261029
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl build
```

The repaired fixtures pass all 28 checks; 18 generated normal requests remain
valid and byte-identical to N-01, and 5,061 real cJSON allocation-fault checks
still pass with zero live allocations. An immutable baseline checkout fails
five fixture checks: two for the stale compression fixture and three for the
overlapping request root. Controlled restoration of none, the exclusive request
root, and a duplicate response branch fail two, three and two checks respectively.
No compile/import failure is counted as a semantic control.

A one-off archived host probe compiles the unmodified actual Word manifest parser
with SDK cJSON and ASan/UBSan/LSan. It accepts the canonical zlib fixture, retains
legacy none acceptance, rejects gzip and rejects a mismatched request ID. It
does not prove physical decompression, network delivery or device timing.

From the cloud worktree's `web` directory:

```sh
./node_modules/.bin/vitest run --cache=false \
  lib/__tests__/study-contract-json-schema.test.ts \
  lib/__tests__/word-study-v1.test.ts \
  lib/__tests__/word-packs-v2.test.ts \
  lib/__tests__/problem-request-json-schema.test.ts
npm run type-check -- --incremental false
npm run lint
npm run format:check
```

Cloud adds root-request acceptance and manifest compression rejection coverage
using strict Ajv2020/formats and actual Zod validation. The group passes 117
tests; whole-project type-check and formatting pass, lint has zero errors and
164 warnings. Firmware builds with zero warnings and passing M8/compiled UI-stack
gates. No endpoint implementation, pack format/version, storage representation
or C++ parser is changed. Device/release acceptance remains deferred. No model/
business API calls, push, PR, deploy or flash occurred. Exact identities, source
probe and logs are archived under
`/home/unknow/projects/doc/robustness-remediation-20261029/n03/`.
