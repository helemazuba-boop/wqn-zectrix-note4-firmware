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

## Batch 3: Editorial sheet, native pixels and truthful metadata

- Status bar contains Todo, the measured clock and shared WiFi/battery icons.
  Date, transient status and pending count occupy the reference's summary.
- Four 46px double-line rows use native 16px titles and 12px metadata, inverse
  focus, page-local ordinals, right-aligned times and explicit overdue marks.
  The title is native 24px; the count uses the existing 32px Latin font.
  Long/control-containing text clips within its column. Empty, loading, error,
  unpaired and completed-current-page views use the same paper geometry.
- A one-bit dashed track and solid thumb report the loaded window. Tiny edge
  arrows indicate remote pages. The global count comes from the server; an
  overdue total appears only when all rows are loaded. No persistent footer.
- ISO due instants account for Z/numeric zones and Beijing day rollover.
  Date-only deadlines retain their calendar date and expire at local day end.
  Invalid dates and an untrusted clock cannot invent overdue items.
- Length-prefixed signatures cover native icon inputs, date, rows and feedback.
- Completing a task changes server offset cursors. The existing worker reloads
  the current window after successful completion, under the same job/mailbox,
  before remote paging can use its cursor. The next row is retained, including
  a newly fetched row at the former page end. If that GET fails, completion
  still succeeds and a boundary press first repairs the current window. No
  cloud API, task, storage protocol, power or panel timing change is required.

Validation: 6350 production Todo/worker/native pixel assertions pass with
ASan/UBSan. The new cursor case fails against the preceding worker at the
missing post-completion page. Navigation 1488, Home 191, Agent 1692 and Note
15639 assertions pass (Note also uses ASan/UBSan). ESP32-S3 build is warning-free
with M8 and compiled stack gates passing. CRLF-aware diff check passes.
Native renderer output is under `doc/assets/ui-todo-firmware-v2`; PGM/PNG
images contain real firmware glyphs, not browser fonts or panel HIL evidence.

Device follow-up: check repeated Up/Down across both window edges, a >24-task
list after completing its last loaded row, completion failure/retry, loading
failure/retry, and global navigation during an in-flight completion. No device
flash or remote push was performed.

Integration: rebased onto the concurrent problem-page commits ending at
`ca0a827`; only Todo hunks were applied in shared files. Production Problem
33090 assertions, Note 15639, navigation 1488 and display-submit 96/0 pass
on the combined sources. The enabled build and compiled stack gate also run
against that combined tree. Todo is validated with mock HTTP ports; real
cloud requests and physical-panel acceptance remain separate.
