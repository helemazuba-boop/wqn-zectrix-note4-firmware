# Agent UI and reading baseline

Approved scope: the Agent picker follows `WQN_Note4_AI_Editorial_v2.html`;
the conversation keeps its current appearance. Fix the recorded AI reading
defects, introduce viewport paging, preserve the confirmed decision gestures,
and indicate actual microphone capture with the existing LED. Notes, problems,
Home and Settings are outside this change.

## Batch 1: picker and status bar

- Four 46px rows with sequence number, one-line title, real update time and
  outcome, right arrow, and whole-row reverse selection.
- The window moves only when selection crosses its edge. Session reload keeps
  the existing id anchor and clamps a removed selection safely.
- Invalid/missing time is omitted. Unknown outcome stays unknown; it is never
  presented as completion. Times use the gateway timestamp in Beijing time,
  without depending on the device's unsynchronized clock.
- Native font measurement and drawing share one font. The status reserves its
  width before fitting the session title; existing icons and edit gestures stay.
- The frame signature includes the window, full outcome and update timestamp.

Validation: production picker/status seams and real font bitmaps: 1470 host
assertions; navigation 1308; Home 139. `build-wsl`: zero warnings, M8 passed;
binary UI entry-frame gate passed. Pixel replays are host output, not panel HIL.

## Remaining batches

2. Bounded shared layouts, complete user prompts, reachable first/last rows,
   consistent native metrics, and synchronized immutable cached results.
3. Agent viewport paging with one-line overlap; retained task/decision gestures;
   input-state presentation and the no-options question correction.
4. Actual-capture LED indication, including both acquisition paths and cleanup.

No push or device flashing is part of the approved local commit delivery.
