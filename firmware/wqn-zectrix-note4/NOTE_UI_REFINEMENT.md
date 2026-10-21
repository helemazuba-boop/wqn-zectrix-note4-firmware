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

## Batch 3: bounded paging and attachment interaction

- Up/Down and body short-confirm move one viewport minus one complete row.
  Short notes and the last viewport stop; they never wrap or jump to pictures.
  Up at the body top opens the last attachment when present, otherwise stops.
- Pictures navigate independently and stop at the first/last attachment.
  Confirm on an error retries that same attachment, retaining the selected
  BW/gray variant. Long-confirm returns to the unchanged body offset and keeps
  real save/sync notices. The error view says how to retry and return.
- Body long-confirm retains the title selection/window. A new input guard
  prevents back/navigation and a new open from clearing the context during
  Prepare -> reserve -> apply; safe body/image paging remains available.
  Persist effect IDs, worker handoff, storage and hardware owners are unchanged.
- Production-state regression covers full-row overlap/coverage, empty/short
  bodies, repeated edge inputs, list windows and typed destinations, retries,
  saved selection, and actual fetch take/apply. A result arriving after moving
  selection, returning, or starting another fetch cannot force the old open.

Validation: 15433 Note assertions under ASan/UBSan; navigation 1488, Agent 1692,
Home 191 pass. Enabled ESP32-S3 build: zero warnings, M8 and binary UI stack
checks pass (owner 1616B, submit 176B, render 4816/4784B). CRLF-aware diff check
passes. Native production pixel samples are local artifacts under
`doc/assets/ui-notes-firmware-v1/batch3`; they are host evidence, not HIL.

## Follow-up: list edge movement forced full refresh (2026-10-19)

Device evidence: `logs/serial-COM7-261019-001158-018-54996.log`, revisions
33-37, uptime 106292-110532ms. Consecutive Down presses were classified as
`commit`, submitting `waveform=2` and rendering `force_full=1`, even with only
1.06-2.21% changed pixels. The four-row window follows its selection one row
at a time, but the old input policy treated every window movement as a full
page replacement. That forced a full waveform on each further edge step.

The Note input policy now uses `selection` for both lists' window movements.
Mode changes and attachment navigation still commit; the existing display
owner's full-frame partial path and periodic cleanup limits remain in place.
Problem UI, panel waveforms, local-partial admission and persist paths are
unchanged.

The production reducer + refresh policy regression fails against the previous
input source at selected index 4 / window 1 (`commit`). It covers forward and
reverse window movement in both lists, repeated real-boundary no-ops, body
paging and retained full refresh for mode/image changes. Note replay passes
15639 assertions under ASan/UBSan; navigation/refresh replay passes 1488.
Enabled ESP32-S3 build (installed ESP-IDF 5.5.6) passes with zero warnings,
M8 and the compiled UI stack gate. Full IDF activation could not find its
OpenOCD/ROM tools; the build used the installed compiler bin directory on PATH
and the existing IDF Python environment explicitly. No debug tools or panel
code were changed. The supplied device log is pre-fix evidence; post-fix panel
behavior still needs device confirmation. Nothing was flashed or pushed.
