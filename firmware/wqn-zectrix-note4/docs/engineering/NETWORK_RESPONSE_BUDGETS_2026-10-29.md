# R-12: bound buffered HTTP JSON and STD session controls

Baseline: `245c962`, branch `fix/robustness-20261029`.

Generic `HttpRequest` previously appended every received byte until EOF or its
read deadline; time did not bound memory. The STD pre-ready control parser also
accumulated incomplete JSON without a byte cap. Its depth-error path queued a
reset and continued, allowing a later ready suffix to publish success first.

`network_response_limits.h` now defines separate byte budgets:

| Buffered input | Budget | Failure behavior |
|---|---:|---|
| Generic HTTP JSON | 512 KiB | Return `ESP_ERR_INVALID_SIZE`, close/cleanup client, release partial body |
| STD pre-ready control | 4 KiB | Owner tears down the connection before accepting any buffered suffix |

HTTP checks the declared length using the SDK's 64-bit result, then checks
every actual chunk before append. Missing/chunked/understated lengths cannot
bypass the cumulative limit, and large declarations cannot wrap through a
32-bit narrowing conversion. Exact-limit bodies are accepted. Read failure and
read timeout also discard the partial body; a new request releases an old output
allocation before starting. Existing status codes and the read deadline remain.

Control overflow and excessive JSON nesting now use the owner's existing fatal
teardown path immediately. Teardown resets the stream/parser, fails the pending
connect, and releases the control string's allocation. Parsing a completed
control object also releases that allocation. A new connection can start cleanly.
This describes owner ordering, not a measured physical socket-close latency.

The HTTP budget is a device wire policy, separate from schema validation. Current
manifests are paged at 100 rows; titles/names/URLs and counters have bounded
schema fields. The host test generates 100-row Word/Note/Problem pages with
maximum title/name, URL length and revision counters and Unicode-escaped wire
strings. Their `manifestData` definitions validate and their 142,089 / 142,602 /
209,405 byte bodies return byte-identically through the real HTTP function. A
24-row Todo size profile with 120-unit titles and 2,000-unit descriptions uses
308,038 bytes and also passes. Todo has no canonical schema assertion here;
this profile reflects current write sanitizers, not all legacy database rows.

The cap intentionally rejects excessive whitespace/escaping or unusually large
legacy payloads rather than returning truncated JSON. Such responses need
server-side trimming/pagination. This is a payload-byte bound, not a guarantee
that all C++/cJSON allocations fit device heap. Multi-megabyte packs and binary
images use other streaming APIs and keep their existing limits.

Run from the application directory:

```sh
python3 scripts/test_stream_transport.py --validate-pages
python3 scripts/test_note_install_serialization.py
python3 scripts/test_manifest_load_preservation.py
python3 scripts/test_agent_voice_finalize.py
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl build
```

The real production HTTP function, WS consumer, parser and teardown compile with
SDK I/O ports, real SDK cJSON and ASan/UBSan/LSan. HTTP buffered input passes
103 checks; WS consumption/teardown passes 322; existing HTTP SSE passes 2,844.
Coverage includes exact boundaries, one-byte fragmentation, unknown/lying/large
lengths, oversize incomplete controls, depth failure plus a ready suffix, clean
reconnect, and release of partial allocations. The baseline fails 36 buffered
HTTP assertions and accepts the oversized control. Four mutation controls
remove the declared cap, cumulative cap, control cap or immediate teardown;
each is rejected by the tests.

The actual Note installer passes 283 host checks, including remote
`ESP_ERR_INVALID_SIZE` preserving the installed manifest/pack and publishing no
partial index. Existing manifest-load preservation passes 87 checks and voice
finalize passes 76. The other domains' callers also return on HTTP error before
parsing/installing (source evidence); this is not a full cross-domain filesystem
or production network test. SDK v5.5.6 builds with zero warnings, M8 ownership
and compiled UI stack gates passing. Device heap pressure, weak networks and
physical storage acceptance remain deferred. No flash, deploy, push or PR.

Separate finding N-03: the existing Word `fixtures/valid/manifest-response.json`
declares compression `none`, while its current pinned schema requires `zlib`.
It fails independent validation. The new size profiles use the actual current
schema; this batch does not rewrite the old fixture or claim live cloud replies
are wrong. N-01's Problem request-schema conflict is also still separate; the
new positive validation above only covers manifest data definitions.

Evidence: `/home/unknow/projects/doc/robustness-remediation-20261029/r12/`.
