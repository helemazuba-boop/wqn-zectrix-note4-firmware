# Home UI: reference A

Implemented from the user's `D:\下载\note4_home_redesign.html`, design A
(墨纸书页). Reference SHA-256:
`50d1f7987f9de2d562367759f7d0a12f86d665889eea53eb2942ca142e3b6a2e`.

The Home screen now places a large clock/date above the current AI task and a
concrete study continuation. Rules and a short vertical focus marker follow the
approved reference; the other pages retain their existing selection language.
The existing long-press navigation menu remains available. AI is the initial
Home focus; short navigation clicks cycle between AI and study, including fast
double clicks. Confirm enters the displayed session or content. Its release
and a following confirm double tap cannot activate the destination page.

## Data and rendering

- AI uses real session identity and outcome. Loading/error/stale transport is
  not treated as successful completion. Explicitly succeeded is complete;
  failed/interrupted are separate. A different current session invalidates the
  previous session's cached status. Opening a completed card records its local
  session/version as opened; this marker is transient.
- The bounded Home query uses the existing Agent worker. It does not detach
  work, change picker selection, clear asks or rewrite conversation history.
  Busy work refuses the query before taking ownership. Network demand and
  command ownership complete before the snapshot is published, on both success
  and failure. Explicit Home entry reuses the picker attachment implementation.
- A list summary supplies running and terminal outcomes, not live permission /
  question details. Those badges require an attached authoritative snapshot;
  Home polling does not promise full background permission notifications.
- Study resumes a loaded problem/note or an eligible default-deck word card.
  Scoped words awaiting reset do not claim resumability. Otherwise Home offers
  the note/problem library. Word progress and its bar use the same numerator /
  denominator, including sequential library positions.
- The footer says 待复习 for local non-mastered problems. Todo count is unknown
  until loaded. The reference's 今日复习 and 逾期 are not fabricated from totals
  or a single timeline page. Pairing instructions remain visible when needed.
- The existing 16px font supplies sized 12/14/20/24px text without a new font
  blob. Coverage preserves thin strokes when reducing size. The clock retains
  the existing 48px numeric assets, whose weight differs from the concept.
- All drawing remains on the EPD owner. Home changes request existing partial
  refreshes; page transitions/cold display/cleanup safety still promote full.
  Clock/date/timer updates redraw the new top region. Menu composition and
  closing restoration retain the whole-page fallback.

## Home standby policy

1. Query once when entering Home while paired and connected.
2. While recent user input keeps Home active and its task is running, query at
   15-second intervals. Idle/terminal states have no continuing periodic query.
3. Stop accepting queries 30 seconds after the latest non-repeat input.
   Replayed older timestamps and held repeats cannot extend this window.
4. New input after inactivity rearms the initial query. Navigation-menu display,
   offline/unpaired state and other pages suppress the Home query.
5. After 45 seconds, failed/offline/stale snapshots display 状态未同步.
6. Queries publish no user activity and install no wake source. Existing deep
   sleep policy remains responsible for sleeping; this checkout's current
   build uses a 60-second idle threshold. An already accepted network operation
   can still defer sleep until its existing bounded transaction completes.

The host runtime simulation verifies two accepted queries over six idle hours
for a running task, then a new query after real input. It does not measure
device power consumption or establish the physical sleep/wake sequence.

## Validation

Run from this firmware project:

```sh
python3 scripts/test_ui_home.py
python3 scripts/test_ui_navigation.py
python3 scripts/test_ui_display_submit.py
python3 scripts/test_ui_stack_gate.py
python3 scripts/test_word_session_start.py
python3 scripts/test_word_cursor_guards.py
python3 scripts/test_word_ram_batch.py
ctest --test-dir build-wsl -R m8_gate_fixtures --output-on-failure
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl build
git -c core.whitespace=cr-at-eol diff --check
```

Home: 88 assertions; navigation: 1308; display submit: 96; stack checker: 17;
word start/cursor/batch: 26/34/32. M8 fixture: 1/1. Three temporary source
mutations (idle cutoff, query cleanup, clicked identity) are rejected by the
Home regression. Host tests compile the actual projection, input shortcut,
query, runtime polling/activity and page/font bodies with worker/network/panel
seams. `--render-dir /tmp/home-render` exports native PBM frames for inspection.

Zero-warning ESP-IDF build and compiled UI stack budgets are required before
commit. No firmware flashing or device HIL has been performed for this change.
Still verify on-device: all Home states and long titles, target-session entry,
image/study restoration, fast confirm tails, menu coexistence, offline recovery,
query completion near idle, actual deep sleep/wake, heap/stack usage, panel
latency and residuals. This change adds no hardware timing, sleep protocol,
storage protocol, persisted screen ID or new NVS key.
