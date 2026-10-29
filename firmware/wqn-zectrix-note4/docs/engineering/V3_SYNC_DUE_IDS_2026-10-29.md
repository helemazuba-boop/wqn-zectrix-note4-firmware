# R-31: enforce the due-problem UUID collection before publishing sync data

Baseline: `2bee739`, branch `fix/robustness-20261029`.

`ParseSyncResponse` previously reserved and copied any number of nonempty
`due_problem_ids`. The pinned contract limits the collection to 100 UUIDs.
An invalid later element could also leave a copied prefix and new checkpoint
fields in the output despite returning an error. Callers currently check the
result before committing; the parser should not expose such a partial result.

The parser now rejects arrays above 100 before vector reservation. It checks
every element for the exact 36-byte, ASCII hexadecimal 8-4-4-4-12 shape before
copying any ID. Empty arrays, uppercase hexadecimal, nil/max UUIDs and repeated
IDs remain accepted; byte order/case is preserved and no deduplication occurs.
Iteration uses cJSON's linked children rather than repeated indexed lookups.

The entire response is parsed into a local candidate, moved to the output only
after all fields and content targets validate. Every parsing failure leaves
the output default-initialized, including the legacy mistake-count sentinel.
The candidate's vectors release their storage on error. This changes no wire
fields or schema hashes and retains existing missing-field/additive-target
compatibility. It is not a complete implementation of every JSON Schema rule.

UUID shape alone is insufficient with cJSON: its decoded strings carry only a
NUL-terminated pointer, so a valid UUID followed by U+0000 and a suffix looks
like the valid prefix. Successful sync envelopes now reject raw embedded NUL
or a real JSON `\u0000` escape anywhere in the body. Escaped literal backslash-u
text remains valid. This deliberately rejects an unrepresentable opaque cursor
too, rather than silently changing its bytes. Other protocol parsers are outside
this guard's scope and are tracked for follow-up as N-04.

Run from the application directory:

```sh
python3 scripts/test_v3_sync_response.py
python3 scripts/test_v3_sync_response.py --before 2bee739
python3 scripts/test_sync_control.py
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl build
```

The new ASan/UBSan/LSan probe compiles the entire production v3 implementation
with real SDK cJSON. It passes 208 checks over 44 wire cases: 0/1/99/100/101/1000
IDs; malformed strings/types/last element; escaped Unicode; raw/escaped NUL;
literal backslash-u; canonical data; legacy/additive compatibility; and a bad
content manifest after otherwise valid IDs. All successful data is checked for
unchanged scalar values and exact ID bytes/order. Error cases verify that no
partial summary, cursor or content target survives.

C++ allocation observation compares rejected due collections with the empty
envelope's one 19-byte temporary allocation. They allocate no additional ID or
vector storage. This is a post-DOM bound: cJSON has already parsed the response;
the separate R-12 HTTP byte budget remains the input-size boundary. It does not
prove total heap sufficiency or recovery from every C++ allocation failure.

The same test fails 107 baseline checks. Five independent mutations remove the
count bound, skip UUID shape, ignore embedded NUL, reserve before validation or
publish directly into the output. All fail semantic/allocation assertions.
The existing sync-control/journal suite passes 51 checks.

The unchanged pinned JSON Schema is independently checked for 42 cases with a
UUID format checker. Two inputs have separate oracles: raw NUL is invalid JSON,
and Python's UUID conversion accepts fullwidth decimal digits. The latter is
explicitly excluded from that oracle; the installed cloud Zod UUID validator
rejects it and accepts the normal uppercase/nil/max controls. Legacy missing
fields, duplicate manifest policy and stricter cursor-NUL rejection are recorded
as intentional parser/schema differences instead of weakening the contract.

SDK v5.5.6 builds with zero warnings; M8 ownership and compiled UI stack gates
pass. The actual Xtensa compiler reports the parser entry frame growing from
192 to 288 B; this is not a device runtime high-water measurement. The sync-service failure branch returns
before `CommitControlResponse` (source evidence); the host parser probe does
not exercise a real backend, filesystem or device checkpoint transaction.

Device acceptance is deferred by instruction. No API/model calls, flash,
deployment, push or PR. Evidence and source/SDK/binary identities:
`/home/unknow/projects/doc/robustness-remediation-20261029/r31/`.
