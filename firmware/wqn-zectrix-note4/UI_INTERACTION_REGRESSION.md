# Home interaction and Settings navigation regressions

## Home: a regional render can acknowledge an unseen selection

Device report: Home's time keeps updating after standby, but visual feedback for
operations stops; switching pages restores it. That device sequence has not yet
been reproduced locally.

A matching source-level failure is reproducible in the host pixel replay:

1. A timer refresh is already pending.
2. A Home selection change joins the same UI iteration.
3. `StrongerSchedule(Timer, Selection)` previously chose Timer because both
   schedules have rank 2 and ties keep the first input.
4. The production frame router paints only Home's clock region. The new focus
   remains invisible, while the display ledger acknowledges the full latest
   signature. Re-entering Home requests a complete composition and reveals it.

`StrongerSchedule` now retains None/identical schedules and the existing
Commit/Immediate priority, but combines different ordinary reasons as Coalesced.
This matches the refresh queue's existing cross-intent policy, so a regional
renderer cannot discard the other reason within one UI batch. Single clock and
timer updates keep their regional paths. The owner render log includes the Home
focus to connect future input/dispatch traces to the rendered frame.

Validation: `test_ui_home.py` passes 191 assertions using the production schedule
combiner, frame router, Home renderer and native font pixels. It checks both
focus directions, reason ordering, clock-only preservation and Commit/Immediate
priority. The pre-fix source fails the new selected-pixel assertion. This proves
the source regression; it is not device standby or panel HIL evidence.

Device acceptance still needed: enter idle on Home, wake and alternate the two
cards, open each card, repeat with a running timer and at a minute rollover,
then repeat while navigation is open. Confirm the focus, destination and clock
all agree; capture the render schedule/focus logs if the device symptom remains.

Both existing Agent-on/off firmware profiles build with zero warnings and pass
M8 plus compiled UI entry-frame gates. Display submission (96), Agent UI (1692)
and the stack-gate fixture (17) also pass.

No sleep policy, power rail, wake source, EPD driver or waveform timing changes
are included.
