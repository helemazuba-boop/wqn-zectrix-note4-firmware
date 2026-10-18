# Problem UI refinement

Approved 2026-10-19: bring the problem title list, status bar, text faces and
self-assessment page onto the implemented Note/Agent UI baseline. Four native
double-line title rows; no persistent help footer; transient, specific save and
sync notices; native 16px body / 20px line height with one complete overlap row.

Keep Confirm's answer-unlock -> self-assessment meaning, the four verdicts,
local durable ACK before advancing, operation-id/persist gates, image transport,
panel owner and waveforms. Ring navigation changes require a separate decision;
this change retains the existing ring and attachment transitions. No flashing,
push, storage protocol, sleep or power changes are authorized.

## Batch 1: math-aware shared layouts

The bounded immutable text cache now includes Markdown options in its identity
and forwards them to the real native layout pass. Problem text can preserve
single `*` and `_` as literal math while AI/Note keep their default rendering.
Regression verifies same-id/text/font option isolation and independent warm
reuse. AI reading: 24432 assertions; Note: 15639, both under ASan/UBSan.
ESP32-S3 enabled build: zero warnings, M8 and compiled UI stack gates pass.
Build evidence is from the existing isolated worktree/build-wsl and the local
ESP-IDF 5.5.6 compiler/Python environment. Host/build evidence is not panel HIL.

## Batch 2: native problem review pages

- The status bar measures the clock/icons and clips its native text to the
  actual remaining width. Text views retain face and problem-position context.
- The title list uses the same four double-line rows, number, arrow, inverse
  focus and edge-following window as Note. Window movements stay on the
  selection/full-frame partial path; mode and ring-segment changes retain
  their existing full-refresh requirement and periodic panel limits.
- Self-assessment is a full native page: set/position, 24px heading, problem
  summary and four dedicated action rows. It has no list-entry arrows or
  persistent help footer. Up/Down selection stays on the partial path.
- Each opened problem builds and retains immutable native body/answer layouts,
  with literal math options. Snapshots share both results and signatures use
  their retained identity instead of copying/hashing entire strings. Counting,
  drawing and bounds use Note's exact 20px-row geometry (13 rows normally,
  12 with an actual notice). Each text step retains a complete overlap row.
- Notices replace list headings or occupy a reserved text row. Specific local
  failures (including full storage) survive scroll/cancel until a new submit;
  ordinary boundary labels do not cause refreshes. Confirm still unlocks the
  answer, then opens the four-option verdict. Successful local ACK advances;
  failure stays on the same problem. The existing ring and image transport,
  BW/gray fallback, persist worker and operation-id gate are preserved.

Validation: production reducer/self-test, full page renderers, real native
font bitmaps and refresh signatures replayed in 33090 assertions under
ASan/UBSan. Includes both long text faces with/without notice at every offset,
cache reuse, same-id edits, saved snapshots, both list ends, all verdict focus
states, cancel/retry/full-storage/advance/end-of-round, and both image paths.
The previous input policy fails at the first list window movement; the new
policy passes. Note regression: 15639 assertions under ASan/UBSan; navigation
and refresh regression: 1488 pass. Native host pixel samples are local artifacts
under `doc/assets/ui-problem-firmware-v1/overview.png`.
Final enabled ESP32-S3 build: zero warnings, M8 and compiled UI stack gates
pass. CRLF-aware diff check passes. These are host/build validations; physical
panel appearance, button timing and post-flash persistence remain device/HIL
checks. No push or flashing was performed.
