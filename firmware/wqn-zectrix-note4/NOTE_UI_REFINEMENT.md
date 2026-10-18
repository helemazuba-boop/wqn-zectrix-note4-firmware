# Note UI refinement

Approved reference: `doc/note4_notes_agent_style.html` (2026-10-18). Preserve
notebook -> title -> body routing, mixed problem/word destinations, the persist
worker and operation-id gate, EPD ownership, and the existing image transport.
No power, sleep, storage protocol or panel waveform changes are in scope.

Three local batches: list/status/feedback; shared native body layout; then
viewport paging and the approved independent image interaction.

## Batch 1: lists and feedback

- Four 46px double-line rows use the Agent picker geometry, sequence, arrow,
  native font metrics and whole-row inverse focus. The shared window moves only
  at its edge and clamps safely when the count shrinks.
- Mixed rows carry a typed kind and use note/problem/word units. Their original
  destinations and ordering are unchanged. Missing content is labelled.
- Title rows separate the title from the real last-viewed date in Beijing time.
  Incomplete candidate lists say loaded and append `+` to the loaded count.
- Native status text reserves the measured clock and shared WiFi/battery icons.
  Visible sync, pairing and save failures replace the list heading. The refresh
  signature includes these visible fields; ordinary boundary hints stay hidden.

Validation: 1956 production snapshot/render assertions with real font bitmaps;
navigation 1488, Home 191 and Agent 1692 pass. The isolated enabled build has
zero warnings and passes M8 plus the binary UI entry-frame gate.
Builds use the isolated checkout `/home/unknow/projects/.wqn-note-ui-verify`
(`firmware/wqn-zectrix-note4/build-wsl`, SDK config copied locally), because the
shared checkout also contained another storage refactor. Only Note changes are
copied for validation; the later storage commit `af6db91` is the clean base.
Host pixels are not physical panel/HIL evidence. No push or flashing requested.

## Batch 2: shared native reading layout

- Body load produces one immutable layout using the existing bounded text cache
  and real native 16px metrics. State and display snapshots retain that same
  result for counting, scrolling and rendering; snapshot creation no longer
  copies the full body or re-hashes it on every scroll.
- Body geometry is shared: 20px rows, 13 complete rows normally, 12 with an
  image entry; an actual notice reserves one further row. First/last bounds and
  the scrollbar use these exact rows. Note images and notices stay outside the
  text viewport.
- Markdown supplies its native font explicitly; problem-page legacy metrics
  remain untouched. Same-id text changes invalidate the layout, old snapshots
  remain readable after eviction/reset, and the safety limit has an explicit
  truncation notice. No storage format or background owner was changed.

Validation: 13564 production load/snapshot/native Markdown assertions under
ASan/UBSan, all scroll offsets with/without image entry and notice, warm reuse
and same-id content changes.
AI reading regression: 24427 assertions under ASan/UBSan. The isolated enabled
build has zero warnings and passes M8 and binary UI entry-frame checks.
