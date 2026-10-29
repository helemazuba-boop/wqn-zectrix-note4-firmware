# N-04: preserve JSON string identity before cJSON parsing

This records the original N-04 batch and its test counts. The subsequent
[N-06 durable repair](DURABLE_JSON_IDENTITY_2026-10-29.md) extends the shared
guard to malformed Unicode escapes and the protocol suite to 166 cases / 266
checks, and completes the preservation-aware local pack/journal follow-up.

Baseline: firmware `2410a6b`. SDK cJSON exposes decoded string values and member
names as NUL-terminated pointers without their decoded lengths. A JSON value
`"req\u0000shadow"` therefore becomes `"req"` when copied to `std::string` or
compared with `strcmp`. The same ambiguity applies to member names such as
`"request_id\u0000shadow"`. Checking UUID shape after parsing cannot recover
the original identity. Raw zero bytes can also hide a suffix from C parsers.

The new host harness reproduces this in the complete v3, Word, Note and Problem
parser translation units with actual SDK cJSON. It also compiles the production
`BootstrapDeviceControlV3`, `FetchWordPackManifest`, `PostAiInteractionReply`,
SSE decoder and provisioning `HandleSubmit` method against host HTTP ports.
Old code accepts truncated request identities and can deliver the resulting
bootstrap/manifest data or invoke the credential-save callback. This confirms
parser and caller impact; it is not evidence of a deployed incident or an
authentication bypass, and no security severity is assigned.

`JsonInputWithinLimits` checks the original bounded bytes for the existing
container-depth budget and for strings cJSON can represent without truncation.
It rejects raw zero anywhere in the supplied span and active `\u0000` escapes,
including unknown fields and member names, before creating a cJSON tree. Escape
parity preserves literal backslashes: `"\\u0000"` stays valid. The Unicode
backslash escape `"\u005cu0000"` does not recursively become another escape.
Other representable control characters and Unicode remain valid. This helper
does not validate all JSON syntax or require complete document consumption;
the existing parsers still perform syntax validation.

The boundary now applies to all four domain protocol documents, decrypted claim
credentials, general WQN HTTP responses, interaction-reply acknowledgement,
STD/Pro control reassembly, SSE decoding, Flash control events, Agent HTTP/frame/
history parsing, and provisioning submission before credential persistence.
Existing depth limits stay 32 by default and 16 for Flash/provisioning. The
provisioning component explicitly declares its private `device_protocol`
dependency. v3 sync's previous private NUL check is replaced by this earlier
shared gate, before envelope/string allocation. Invalid HTTP 200 data does not
clear pairing state. No model/provider settings or cloud contract are changed.

From the firmware application directory:

```sh
python3 scripts/test_json_string_boundaries.py
python3 scripts/test_json_string_boundaries.py --before 2410a6b
python3 scripts/test_json_string_boundaries.py --mutation escaped-nul
python3 scripts/test_v3_sync_response.py
python3 scripts/test_flash_events.py
python3 scripts/test_stream_transport.py
python3 scripts/test_agent_run_progress.py
python3 scripts/test_sync_control.py
python3 scripts/test_ws_release_deadline.py --tick-hz 100
python3 scripts/test_ws_release_deadline.py --tick-hz 1000
/tmp/wqn-robustness-schema-venv/bin/python scripts/test_protocol_request_oom.py \
  --validate-schema --cloud-root /home/unknow/projects/.worktrees/WQN-robustness-20261029
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl build
```

The dedicated harness has 124 protocol wire cases and 212 assertions, including
actual HTTP delivery, SSE, bounded raw-zero positions, slash parity and real
provisioning submit/callback behavior across 1/17/512-byte reads. Baseline fails
132 assertions; repaired code passes all 212 under ASan/UBSan/LSan. Note response
fixtures are explicitly synthetic minimal valid inputs; the other domains use
canonical contract fixtures. No real credentials or network are used. Six
mutations are rejected by semantic failures: remove raw-zero rejection, remove
escaped-zero rejection, break escape parity, bypass the Word gate, bypass the
SSE gate, and bypass the provisioning gate. Compilation failures are not counted.

Regressions pass: Flash parser/history 49, Agent client/stream 167, HTTP SSE 2,844,
STD/Pro consumer 352, buffered HTTP 103, sync control 51, and ASR handoff owner
166 at each of 100/1000 Hz. v3 sync retains 44 wire cases / 208 assertions and
five effective semantic mutations. Its allocation upper-bound assertion now
permits fewer allocations: rejecting before parsing allocates less than a valid
empty envelope; ID/vector growth is still prohibited and reserve-before-check
mutation still fails. Protocol builders pass 5,061 allocation-fault checks with
zero live allocations; 18 normal requests and all 14 Word wire fixtures retain
Schema validity. Full SDK build has zero warnings and passes M8 and compiled
UI-stack gates. Decrypted claim handling is compiled in that build; this batch
does not execute the complete cryptographic exchange on host or hardware.

Local persistence deliberately keeps `JsonNestingWithinLimit` unchanged. Pack
readers in `word_pack.cpp`, `note_pack.cpp`, and `problem_pack.cpp` have C-string
length boundaries; `storage.cpp` and `problem_store.cpp` have their own durable
decode/recovery policy. In particular, `DecodeObservationLine` maps a guard
failure to malformed, and rewrite callers may drop malformed records. Replacing
that guard without a preservation decision could lose a syntactically valid
record. These pack/journal identity, original-length and recovery questions are
tracked separately as candidate N-06, requiring actual parser/I/O/rewrite
reproduction. N-04 closes the external protocol/control scope above; it does not
claim every local JSON decoder is repaired. No pack version, journal format or
storage repair policy changes in this batch.

Device/HIL/release acceptance remains deferred under the user's staged repair
instruction. Full document consumption, physical transports, provisioning/NVS,
crypto interoperability and device stack high-water marks remain distinct
acceptance boundaries. No model API call, push, PR, deploy or flash occurred.
Logs, generated cases, semantic controls and exact source/build identities are
archived in `/home/unknow/projects/doc/robustness-remediation-20261029/n04/`.
