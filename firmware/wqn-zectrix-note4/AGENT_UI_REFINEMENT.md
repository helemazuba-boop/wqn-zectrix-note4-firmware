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

## Batch 3: Agent paging and decision feedback

- Agent pages by the available viewport, reserving a complete overlap row even
  with partial rows at both edges. The backend still owns/clamps the offset;
  an edge press returns no refresh and does not retire follow unnecessarily.
- Keep existing follow/manual-reading rules, modal decision priority, picker
  gestures, discard-on-reinput and interrupt-on-custom-answer behavior.
- Retain the pending bubble and horizontal two-option window. Its title now
  uses a real 12px row with a gap above the body. Options use measured available
  widths and inverse selection; their counter stays clear of the scroll arrow.
- An empty partial transcript shows recognition progress. A question without
  selectable options directs the user to choose 自己回答 before new input,
  instead of advertising a hold gesture that the modal itself consumes.

Validation: 1692 production Agent drawing/input assertions; reading 24427,
including complete coverage and overlap across a 100-row paged answer; Agent
round policy 1220. `build-wsl`: zero warnings; M8 and binary UI entry-frame gates
passed. Pending/options host pixel replays inspected; panel HIL remains separate.

## Batch 4: actual recording LED

- The existing GPIO3 LED is active-low. A platform HAL level-write function is
  reached only through AudioService's Capture/Flash capability-checked API.
- Standard/Agent capture lights it after codec/I2S warmup succeeds; Flash lights
  it on its first valid post-warmup PCM read. Admission and connecting stay dark.
- Release, cancellation, initialization timeout, read failure and cleanup turn
  it off. A late warmup/read cannot re-arm it after release/error. Flash keeps
  its duplex channels for TTS without keeping the recording LED lit.
- Feature mutexes serialize readiness/stop, with stable indicator token copies.
  The existing driver inflight guard prevents session teardown overtaking an
  indicator write; stale capabilities cannot change a newer session's LED.
  EndActivity adds a final off fallback after driver operations drain.
- No polling task, timer, wake source, GPIO hold policy, rail timing, sleep
  protocol or PowerCoordinator change was introduced. Existing sleep-off stays.

Validation: 67 production LED/capability/lifecycle assertions under ASan/UBSan;
audio runtime 75 and RX DMA 98. `build-wsl`: zero warnings; M8 and binary UI
entry-frame gates passed. Physical LED timing, microphone quality, EPD paging
latency and device heap/stack peaks still require device observation; host ports
are not HIL evidence.

## Final regression

Both existing profiles, `build-wsl` (Agent enabled) and `build-agent-off`, build
with zero warnings and pass M8 plus the compiled UI entry-frame gate. Reading
24427 assertions also pass under ASan/UBSan. Navigation 1308, display submission
96, stack-gate fixture 17, Home 139, Settings 235/250 and word UI 292 all pass.
These are host/build checks; no device flashing or HIL was performed.

No push or device flashing is part of the approved local commit delivery.
