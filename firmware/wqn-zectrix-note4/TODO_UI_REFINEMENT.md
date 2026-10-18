# Todo UI refinement

Reference: `doc/WQN_Note4_Todo_Editorial_v2.html` (2026-10-19).
Scope: Todo list/rendering/input/feedback only. Preserve cloud runner and ACK
mailbox ownership, global navigation, storage protocol, power and panel timing.
Development and builds use the independent `.wqn-todo-ui-work` worktree to
avoid mixing the concurrent problem-page work. Host checks are not device HIL.

## Batch 1: stable selection and cloud paging

- A four-row window moves only across its edges in either direction. Renderer,
  model projection and selection clamping use the same window rule.
- Ordinary refresh anchors selection by task ID; explicit cursor navigation
  follows the server's first/last selection even when page content overlaps.
- Todo input returns selection refresh for local movement, no refresh at true
  boundaries, and rejects duplicate cursor requests. Failed loads retain the
  previous page and cursor for retry; an empty loaded page retains both remote
  directions after its final task is completed.
- The visible signature includes the window and available remote directions.

Validation: production Todo input/cloud seams pass 359 assertions under
ASan/UBSan. ESP32-S3 build has zero warnings and passes M8 and compiled UI stack gates.
Navigation 1488 and Home 191 assertions pass.
No push, flashing or device HIL is part of this change.
