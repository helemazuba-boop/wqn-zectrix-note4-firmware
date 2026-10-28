# R-28: fail closed while constructing protocol requests

Baseline: `1e0d71dcb32169acad19b01b1cfd9f6dc094c974` on
`fix/robustness-20261029`. This is the network encoder repair from the general
2026-10-27 robustness audit, separate from earlier disk-codec repairs.

Under cJSON allocation failure, the v3, Word, Note and Problem request builders
could return `ESP_OK` with missing fields. Failed object attachment also left
unowned child trees allocated. An incomplete observation could reach the server,
be rejected as `INVALID_REQUEST`, and be parked by the existing outbox policy.
The latter consequence was traced in source; this repair makes no production
request and does not alter existing parked records.

All allocating field additions now check their result. Containers use SDK
`cJSON_AddArrayToObject` / `cJSON_AddObjectToObject`, which delete the child when
key allocation fails. Array strings are freed unless attachment transfers
ownership. The root's existing RAII owner reclaims every attached child on any
early return. Sync builds one complete tree and renders once, eliminating an
intermediate body publication and a redundant render/parse pass.

For valid inputs, every tested cJSON allocation failure returns
`ESP_ERR_NO_MEM` and preserves the caller's previous output. Normal wire bytes
and field order remain unchanged. This scope covers cJSON allocation failures;
it does not provide a general recovery guarantee for C++ container allocation
failure or sustained whole-device heap exhaustion.

Run from the firmware application directory:

```sh
python3 scripts/test_protocol_request_oom.py
python3 scripts/test_sync_control.py
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl build
```

The first test compiles the four production translation units and the SDK's
real cJSON with ASan, UBSan and leak detection. It covers 14 public builders with
18 success cases, populated/empty optional fields, nested arrays and safe-integer
boundaries. Each allocation is failed once and then persistently from the same
point, including final serialization. Invalid input and null output are checked
separately. The fixed source passes 1,244 injected scenarios / 5,061 assertions
with zero live cJSON allocations. The same test against the baseline fails
1,162 assertions and leaks 434 allocations (host byte counts are not ESP32 heap
estimates). Sync allocation attempts drop from 73 to 37 for this fixture.

All 18 normal request bodies are byte-identical to baseline. Sync control's
51 host checks pass. ESP-IDF v5.5.6 builds successfully with zero warnings,
M8 and compiled UI stack gates passing (image `0x383e90`, 11% partition free); a host test or build does not constitute device HIL or release acceptance.

`--normal-json FILE` writes synthetic normal requests;
`--source-root PATH` tests an older checkout with the same public interfaces.
`--validate-schema --normal-only` is an additional, independent contract check.
At this checkpoint it passes 15/18 fixtures and correctly fails all three Problem
fixtures: that pinned schema combines an `allOf` reference to closed metadata
with additional request fields. This pre-existing schema defect remains a
separate follow-up; the test does not weaken the schema to report success.

In `main/wqn_api.cpp`, all three observation wrappers return immediately on
builder error, before `HttpRequest`. They initialize the protocol error to
empty; the existing `ClassifyServerErrorCode` / `ClassifyOutboxFailure` path
classifies this as a transient retry, without suspension or deletion. This is
source evidence, not an end-to-end network/durable-storage fault test. Previously
parked observations still require a recovery policy that respects their stored
reason and idempotency identity; they are not automatically replayed here.

No device flash, production traffic, push, PR, deployment or physical validation
is performed in this batch. General-audit R-01/R-06 and the remaining voice,
display and buffer issues remain open. The execution archive lives outside the
repository at `/home/unknow/projects/doc/robustness-remediation-20261029/r28/`.
