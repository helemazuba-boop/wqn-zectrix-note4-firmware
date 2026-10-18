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

## Batch 2: operation-bound completion and accurate refreshes

- Only an admitted completion sets the completing state. Sync busy or duplicate
  input keeps the accepted operation's state; re-entry cannot overwrite it.
- Capture task ID/title for the pending operation, freeze local selection while
  submitting, retain failed tasks for Confirm retry, and remove only the
  acknowledged task. Unrelated/duplicate results and mismatched response IDs
  cannot remove another row or decrement the count twice.
- Completion submission/error feedback uses selection refresh. Moving after a
  terminal result retires its hint. Global navigation remains independently
  available through its earlier input bridge.
- Refresh failures retain the known global count. Subject and completion-date
  changes participate in page comparison so visible updates are installed.

Validation: 420 production Todo assertions pass under ASan/UBSan. ESP32-S3
build has zero warnings and passes M8 and compiled UI stack checks.
