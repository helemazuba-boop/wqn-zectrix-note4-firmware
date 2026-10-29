# N-06, part 1: pack text framing and JSON identity

Baseline: firmware `47836e7`. N-06 is confirmed and only partly repaired by
this batch. The Word/Note/Problem pack paths are repaired locally; the problem
outbox and sync journal require a separate preservation-aware repair.

The dedicated host harness compiles the actual three bounded line readers,
row parsers and complete pack-to-index scans with SDK cJSON. Synthetic pack
files demonstrate accepted ID/member-name/metadata prefixes containing
`\u0000`, including a bad second record after a valid first record. Note and
Problem accept EOF-terminated final rows; their old `fgets`/`strlen` readers can
accept a complete JSON prefix followed by a raw zero and hidden suffix at EOF.
This is demonstrated at the file/scan boundary, not inferred from a lexical
search. No production corruption or exploit is claimed.

`ReadBoundedPackTextLine` now measures bytes actually consumed from the binary,
seekable pack file using its before/after logical offsets. Existing index
reads already require seekable `ftell`/`fseek` files. It rejects observed raw
zero bytes before constructing text, checks stream errors, and rejects failed
position measurement. It keeps the existing content byte limits: Word requires
LF; Note/Problem may end their final line at EOF. CRLF remains supported. The
line buffer grows by one byte so a maximum-sized content line plus CRLF and
the C terminator fits; the previous allocation incorrectly rejected that edge.
Over-limit lines remain rejected. Buffer reuse and subsequent record offsets
are checked against real libc files.

The three local `JsonDocument` wrappers take `std::string_view` and use the
N-04 representability/depth gate plus `cJSON_ParseWithLength`. Stored manifest
payloads and metadata strings retain their complete lengths instead of being
converted to C strings first. No additional row-sized string copy is needed.
The private row parsers and public Problem fixture parser retain their textual
C-string interfaces; file callers reach them only after the bounded reader has
rejected raw zeros. Their active `\u0000` escapes are rejected by the JSON gate.
This does not turn a C-string API into a general raw-byte API. Literal escaped
backslashes, Unicode and other representable escaped controls remain valid.

An invalid pack scan uses the existing entry rollback guard: entries appended
from that pack are removed while earlier packs' entries remain. Tests start
with an existing sentinel entry and verify it survives invalid metadata and
late invalid rows. Every test verifies the original pack file is byte-identical
after scanning. This is not a promise that a higher-level index contains no
other usable packs or that manifest publication/network download is atomic.

Stored-manifest pruning tests additionally inject NUL values, member names,
unknown extension values and raw document suffixes into backup manifests in
all three domains. Failed parsing defers cleanup and retains the current,
backup and otherwise stale offline packs. Tests use the actual parsers and
prune functions with real temporary files. Existing malformed-manifest recovery
policy is preserved; no storage format/version changes.

From the firmware application directory:

```sh
python3 scripts/test_pack_json_boundaries.py
python3 scripts/test_pack_json_boundaries.py --before 47836e7
python3 scripts/test_pack_json_boundaries.py --mutation raw-span
python3 scripts/test_pack_prune.py
python3 scripts/test_manifest_load_preservation.py
python3 scripts/test_note_install_serialization.py
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl build
```

The dedicated suite passes 189 assertions under ASan/UBSan/LSan. Baseline fails
49 assertions, including framing/status/identity/rollback expectations; these
are not 49 distinct defects. Five semantic mutations are rejected: bypass raw
span checking, bypass each domain's JSON gate, and disable the existing scan
rollback. Pruning passes 32,484 assertions with 740 encoder allocation-fault
scenarios. Manifest-load preservation passes 87, Note installation orchestration
283. Full ESP-IDF build passes with zero warnings and passing M8/compiled
UI-stack gates. The offsets add two `ftell` calls per line; physical SPIFFS/
newlib latency and task high-water marks still need device acceptance. This
batch does not claim a throughput improvement or prove physical power-loss
recovery, download SHA verification or renderer behavior.

Separately archived probes establish remaining N-06 impact using actual durable
codecs and transactions. Four Problem outbox cases (request value/key, problem
ID and timestamp) decode/peek as the truncated prefix; an ACK for that prefix
removes the original raw record during rewrite. Two sync-journal cases load a
NUL-aliased cursor key or truncated blocked-image value and subsequently rotate
that primary as if valid. These are observations of unfixed behavior. The
existing outbox and journal regression suites still pass 4,716 and 34 checks;
their green status does not validate NUL identity or close N-06. The standalone
probes are archived rather than committed as tests that bless the bad result.

`storage.cpp`'s generic `JsonDocument` class has no construction call sites in
the current file; the live sync-journal parser is the relevant path. Problem
outbox decode failures can trigger malformed-record deletion during rewrite,
so merely replacing its depth-only guard would be insufficient. That remaining
repair must distinguish unrepresentable records from known corrupt frames and
preserve durable bytes through all query/ACK/park/append/recovery paths. This
batch changes neither `problem_store.cpp` nor `storage.cpp` and does not mark
the overall follow-up repaired.

Logs, probe adapters and source/build identities are under
`/home/unknow/projects/doc/robustness-remediation-20261029/n06/`. All inputs are
synthetic; no real credential, model call, push, PR, deploy or flash occurs.
Device/HIL/release acceptance remains deferred under the staged repair
instruction. Overall robustness remediation remains active.
