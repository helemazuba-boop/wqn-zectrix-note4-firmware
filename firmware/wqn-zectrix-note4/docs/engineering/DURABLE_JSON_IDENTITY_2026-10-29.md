# N-06, part 2: preserve durable JSON identity

Baseline: firmware `e9a9a48`. This completes the local repair of the durable
Problem outbox and sync-journal paths left open by
[the pack batch](PACK_JSON_IDENTITY_2026-10-29.md). Device/release acceptance
remains pending. Tests use synthetic files and the actual codecs/transactions;
they do not establish a production data-corruption incident.

SDK cJSON loses decoded string lengths. Both `\u0000` and malformed hexadecimal
escapes such as `\uZZZZ` can become zero bytes, so comparison against a request
ID, field name or image ID can silently accept its prefix. The shared bounded
preflight now rejects zero and malformed/truncated four-digit Unicode escapes
before cJSON parsing. Literal escaped backslashes remain valid. The parser
still owns other JSON syntax. This strengthens N-04's protocol/control boundary
as well as protecting packs and durable records; no cloud contract changes.

The Problem outbox now distinguishes a complete but unrepresentable record from
known corrupt syntax and temporary decode-allocation failure. The existing
allocation-free, bounded syntax probe moves to `device_protocol` as
`JsonSyntaxProbe`, using a non-owning string view. It retains the tested cJSON
legacy syntax behavior so OOM cannot reclassify readable records as disposable.

Unrepresentable outbox rows stay opaque in `po_outbox.jsonl`. Reads retain their
payload bytes and LF/CRLF framing; no prefix identity is published. Peek skips
them and continues to later valid records. Append, idempotence, ACK, quarantine,
parking, repair rewrites and interrupted-publication recovery retain them.
They count toward the same 200-record capacity and the existing suspended
count, which the Problem UI displays as `同步挂起 N 条`. They cannot falsely
satisfy a prefix ACK or acquire a prefix park marker. A valid observation with
that prefix remains a distinct record and can flow normally. Opaque-only data
never establishes the physically-empty cache. The producer also rejects an
embedded zero in a timestamp before C-string encoding can truncate it. Queue
probes avoid a warning per opaque row, keeping retained records from flooding
the serial log. These rows require identity recovery from their original bytes
before they can be uploaded; they are not automatically normalized or deleted.

For the sync journal, an unrepresentable primary returns
`ESP_ERR_NOT_SUPPORTED` without publishing its cursor/identity or choosing an
older backup as an implicit substitute. A supported current primary remains
readable when its older backup is opaque. Before creating/truncating staging
data, the writer checks both durable sources: it refuses to rotate/remove an
opaque primary or backup. Supported source recovery permits a normal retry.
The existing fallback and rotation for known corrupt frames remains in place;
raw-zero corruption follows that existing policy. Unsupported escaped strings
require deliberate checkpoint recovery before synchronization can advance.

The same review exposed a related OOM problem: cJSON's null result could make
a valid latest primary look corrupt, causing fallback or destructive rotation.
If parsing fails but `JsonSyntaxProbe` establishes a complete document, the
decoder now returns `ESP_ERR_NO_MEM`. Load and preflight propagate that result
without fallback or file mutation. The syntax probe allocates no memory and
does not replace global cJSON allocator hooks. This does not guarantee recovery
from every C++ allocation failure, media fault or non-returning storage call.

The journal's existing coalescing cache assumes the StorageService is the sole
writer and caches only successful durable saves. Repeated identical saves still
use that cache. The repair does not add continuous media-integrity polling;
restart/uncached load/write tests cover the durable-source classification.
File formats, schema versions and park-marker syntax are unchanged. Baseline
firmware retains its demonstrated unsafe decoding behavior for these inputs.

From the firmware application directory:

```sh
python3 scripts/test_problem_outbox_recovery.py
git show e9a9a48:firmware/wqn-zectrix-note4/main/problem_store.cpp > /tmp/wqn-n06-problem-before.cpp
python3 scripts/test_problem_outbox_recovery.py --source /tmp/wqn-n06-problem-before.cpp
python3 scripts/test_sync_journal.py
python3 scripts/test_sync_journal.py --before e9a9a48
python3 scripts/test_problem_empty_cache.py
python3 scripts/test_sync_control.py
python3 scripts/test_json_string_boundaries.py
python3 scripts/test_json_string_boundaries.py --before e9a9a48
python3 scripts/test_json_string_boundaries.py --mutation bad-hex
python3 scripts/test_pack_json_boundaries.py
python3 scripts/test_stream_transport.py
python3 scripts/test_flash_events.py
python3 scripts/test_agent_run_progress.py
python3 scripts/test_v3_sync_response.py
python3 scripts/test_ui_problem.py --sanitize
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl build
```

Validation uses real SDK cJSON with ASan/UBSan/LSan for the durable and general
JSON suites. Outbox passes 4,971 assertions, including ten opaque variants with
LF/CRLF, maximum-length rows, 200-record capacity, normal progress, prefix ACK/
quarantine/parking, failed publish/restore, short write, failed close, restart
and corrupt-tail repair. It retains 377 transaction-allocation faults, 310
representable legacy allocation faults and 457 accepted syntax mutations.
Baseline outbox source fails 205 assertions under the new harness; totals vary
because unsafe strings no longer enter the allocation-heavy decode path.

Sync journal passes 621 assertions. Eight alias variants cover primary/backup
roles, absent primary, staging preservation and source recovery. Each of 132
real primary-decode allocation sites is failed separately for load and write;
both generations and an existing staging sentinel remain byte-identical.
Baseline fails 436 assertions. The shared protocol suite expands to 166 wire
cases / 266 assertions; the former guard fails 54 malformed-hex assertions.
All 27 semantic controls are rejected: 13 outbox, seven journal and seven
protocol controls. Compile failures are not counted as effective controls.

Regressions pass: empty-cache/I/O 58, sync service/control 55, pack scans 189,
v3 sync 208, HTTP SSE 2,844, STD/Pro consumer 352, buffered HTTP 103, Flash 49,
Agent 167 and Problem native UI 33,090 assertions. The service checks confirm
unsupported/OOM errors cannot mark startup ready or invoke a control RPC, then
recover normally. The UI check exercises the existing suspended-count display;
physical panel rendering is still HIL. Full SDK build has zero warnings with
passing M8 and compiled UI-stack gates. Physical SPIFFS/newlib failure timing,
power loss, task high-water marks and device/release acceptance remain deferred
under the user's staged repair instruction. No model call, push, PR, deploy or
flash occurred. Evidence and exact identities are archived under
`/home/unknow/projects/doc/robustness-remediation-20261029/n06/durable/`.
