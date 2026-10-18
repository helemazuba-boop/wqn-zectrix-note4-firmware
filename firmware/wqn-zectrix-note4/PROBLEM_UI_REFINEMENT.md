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
