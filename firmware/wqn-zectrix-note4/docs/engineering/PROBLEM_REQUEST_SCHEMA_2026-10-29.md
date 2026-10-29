# N-01: Problem requests must validate without reopening metadata

Firmware baseline `5482509`; cloud baseline `91ce1f2`. This is a published
JSON Schema/coverage defect, not evidence that production routes rejected
these normal requests. The routes already validate the complete request with
Zod's extended strict object.

Both `manifestRequest` and `observationRequest` previously used `allOf` against
`requestMetadata`, whose `additionalProperties: false` rejected every business
field outside its own four metadata fields. Thus normal manifest/observation
requests failed even though their request-specific `properties` were declared.
The former cloud JSON Schema suite only checked Problem pack row/meta fixtures;
its request tests used Zod and could not catch the contradiction.

Each complete request now declares its full required/property set and closes
unknown fields at that level. Metadata properties refer to the original
`requestMetadata/properties/...` leaf definitions, so their constraints remain
shared. The standalone metadata object remains closed. No metadata or business
field constraint, response/pack definition, wire version or serialized request
byte is changed. This removes the incompatible whole-object intersection
instead of deleting the extra-property protection.

The schema digest changes from
`e7b3419f74c2e234b857ecbaf47519e09306ec9f37875a46eebc9a7fb61db87d`
to `dfb8b016a5fa747014f656a8c195bf6e5a2b0df087637cc04ae767071a31e32e`.
Both manifests, mirrored schemas and the cloud exported digest pin are updated
together. ESP-IDF's existing configure dependency/hash gate injects the new
digest; no manual C++ code generation is needed. The cloud-only README is
explanatory documentation; all 12 JSON contract artifacts are byte-identical.

The shared `fixtures/request-validation-cases.json` has 83 cases (19 valid,
64 invalid). It covers normal/minimal/complete manifests, every review action,
required metadata/business fields, metadata type/length/pattern bounds, paging
and snapshot limits, invalid timestamps, null requests, unknown fields, fields
from the other request shape, and standalone metadata's closed boundary.

`scripts/test_problem_request_contract.py` checks both the selected definition
and, for complete requests, the root wire union: 164 checks. Cloud
`problem-request-json-schema.test.ts` consumes the same cases with strict
Ajv2020/formats and the actual route Zod schemas, also checking the root union.
These selected cases agree across all three validators. This does not claim
exhaustive equality for every possible JSON Schema/Zod edge case.

Python jsonschema can silently omit date-time validation when its optional
checker dependency is absent. The new test explicitly refuses that environment.
An isolated test environment was used without changing system Python packages:

```sh
python3 -m venv --system-site-packages /tmp/wqn-robustness-schema-venv
/tmp/wqn-robustness-schema-venv/bin/python -m pip install rfc3339-validator==0.1.4
/tmp/wqn-robustness-schema-venv/bin/python scripts/test_problem_request_contract.py \
  --cloud-root /home/unknow/projects/.worktrees/WQN-robustness-20261029
/tmp/wqn-robustness-schema-venv/bin/python scripts/test_problem_request_contract.py \
  --before 5482509
python3 scripts/test_protocol_request_oom.py --validate-schema
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl build
```

The second schema command intentionally rejects the old schema with 36 failed
semantic checks; it uses the same current cases and unchanged wire fixtures.
The isolated environment had jsonschema 4.10.3 and rfc3339-validator 0.1.4.
Missing-checker refusal was separately observed with the original environment.
Six mutations restore the closed allOf, reopen either complete request, remove
required metadata/cursor, or unconstrain a metadata leaf. All six are rejected
by semantic assertions; an import/checker/harness failure is not accepted as
rejection evidence.

The real cJSON request-builder suite passes all 18 normal requests against the
pinned schemas (formerly 15/18), and its 5,061 allocation-fault checks retain
zero live allocations. All 18 serialized JSON lines are byte-identical to the
R-28 repaired baseline. The schema's other definitions and original business
property constraints were compared directly with the baseline and are unchanged.

In the isolated cloud worktree, matching baseline dependency locks were verified
before reusing installed node_modules. No dependency manifest/lock is modified.
Run from its `web` directory:

```sh
./node_modules/.bin/vitest run --cache=false \
  lib/__tests__/problem-request-json-schema.test.ts \
  lib/__tests__/study-contract-json-schema.test.ts \
  lib/__tests__/problem-study-v1.test.ts
./node_modules/.bin/vitest run --cache=false \
  lib/__tests__/problem-review-service.test.ts \
  lib/__tests__/problem-review-route.test.ts \
  lib/__tests__/problem-packs.test.ts
npm run type-check -- --incremental false
npm run lint
npm run format:check
```

The two cloud groups pass 96 and 17 tests. Whole-project type-check and format
checks pass; lint has zero errors and 164 warnings. Firmware builds with zero
warnings and passing M8 ownership/compiled UI-stack gates. These are local
contract/build results: remote CI, production API behavior and device end-to-end
acceptance are not established by them. No push, PR, deployment, flash or model/
business API call occurred. Exact source/commit/config/build identities and
logs are archived under
`/home/unknow/projects/doc/robustness-remediation-20261029/n01/`.
