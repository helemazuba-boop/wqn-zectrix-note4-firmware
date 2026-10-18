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

## Batch 2: complete reading and reusable layouts

- Outward-rounded scroll limits make the first and last complete rows reachable,
  including content overflowing by less than one row. Indicator clearing no
  longer erases the right end of the last row.
- User prompts retain their full wrapped text instead of stopping at four rows.
  A 4096-row safety limit produces an explicit truncation notice.
- AI plain text and Markdown measure and draw with the same native font. The
  optional Markdown font preserves legacy note/problem layout behavior.
- Immutable per-message results use a mutex-protected 32-entry / 1 MiB accounted
  cache. Three immutable snapshot layouts avoid repeating layout on paging;
  revised replies reuse unchanged prompts. Eviction cannot invalidate readers.

Validation: 24212 production reading assertions, repeated under ASan/UBSan;
Agent UI 1470, navigation 1308, Settings 235/250. The reading cases cover partial
row overflow, long prompts, all scroll positions of mixed Markdown, tail pixels,
cache limits and concurrent readers. `build-wsl`: zero warnings, M8 and binary
UI entry-frame gates passed. Host pixel replays do not verify the physical EPD.

## Remaining batches

3. Agent viewport paging with one-line overlap; retained task/decision gestures;
   input-state presentation and the no-options question correction.
4. Actual-capture LED indication, including both acquisition paths and cleanup.

No push or device flashing is part of the approved local commit delivery.
