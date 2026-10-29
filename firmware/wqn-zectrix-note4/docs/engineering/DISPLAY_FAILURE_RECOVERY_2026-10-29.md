# R-10: bound display failures across frame intents

Baseline: `eef967c`, branch `fix/robustness-20261029`.

Every Failed display result previously generated a new immediate full-refresh
revision. The driver's one recovery inside a transaction did not limit that
cross-intent loop. New clock/content frames and an already accepted secondary
frame could also enter hardware without a failure backoff.

`DisplayRecoveryPolicy` is shared under the existing refresh mutex. The EPD
owner updates it immediately after actual render completion and checks it again
before starting each transaction. It permits the initial render, then three
automatic retries after 1, 5 and 15 seconds from each failed completion. A fourth
consecutive failed transaction pauses automatic rendering. A successful render
clears the streak; recovery uses the Full/commit drawing path, which disables
identical-signature deduplication as evidence of recovery.

The producer rejects work during backoff or pause before acquiring a lease or
reserving a result slot. An older accepted primary/secondary intent reaching the
owner during that window receives Failed without hardware; it still follows
the existing result/ACK lifecycle and does not consume another failure attempt.
The owner drains its work and can service idle/sleep preparation normally.
Long retry waits hold no display lease or queued frame. The driver's internal
bounded recovery, idle-maintenance and sleep-prep logic are unchanged.

The UI retains the full-refresh obligation and latest logical state while
blocking automatic submissions. Its retained-standby wait includes the next
retry deadline; it does not spin on a rejected request or hold a lease through
the backoff. When delivery reopens, a typed recovery event supplies a fresh
revision and the newest pixels. Existing event enum values remain stable.

After pause, a fresh non-repeat physical `kPress` produced after the latest
pause requests one probe. Stale queued events and held-key repeats cannot grant
it. A one-second post-failure cooldown coalesces rapid presses. That probe does
not reset the automatic budget: failure returns to pause, and successful Full
rendering restores ordinary updates. Existing press/release/business gesture
delivery continues. Pause is recorded through `RecordError` and structured logs;
`CopyDisplayRecoveryStatus` exposes the bounded RAM state without storage I/O.
Policy survives retained standby and resets on a new boot.

Run from the application directory:

```sh
python3 scripts/test_display_recovery.py
python3 scripts/test_display_recovery.py --before eef967c
python3 scripts/test_ui_display_submit.py
python3 scripts/test_epd_idle_scheduler.py
python3 scripts/test_epd_refresh_windows.py
python3 scripts/test_ai_force_full.py
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl build
```

The ASan/UBSan/LSan replay compiles the complete production EPD owner task,
submission/merge/outstanding ledgers, ACK/result tracking, real UI recovery
helpers and revision reducers. RTOS/time, rendering, leases and error recording
use controlled ports. Source checks also bind the helpers to the real UI input,
render and poll-wait callsites. The full DeviceUiTask and physical panel are
outside this probe.

It passes 715 checks at each of 100/1000 Hz. The virtual zero-duration renderer
starts at 100, 1,100, 6,100 and 21,100 ms, then parks. A traffic variant adds
secondary/superseding frames during rendering; blocked accepted work terminates
without more hardware calls. Coverage includes 1,000 paused content changes,
direct-producer rejection, terminal/ACK/lease/watchdog balance, deferred latest
pixels (including changes during a manual probe), stale/repeated input, one failed manual probe, cooldown and successful
recovery, transient failure, long monotonic uptime and counter saturation.

The old owner reaches the probe's finite observation cap of 20 immediate
render calls with no pause; that cap is test control, not an old runtime bound.
Seven mutations remove the limit/backoff, bypass owner/producer/UI gates,
renew the automatic budget on a probe, or accept stale presses. All fail
semantic assertions at both tick rates. Existing submission/merge passes 96
checks, idle/sleep-prep scheduler 677 at each rate, SPI window paths 68, and
AI force-full delivery 57.

SDK v5.5.6 builds with zero warnings; M8 and compiled UI stack gates pass. The
UI owner frame remains 1,872 B; request submission is 192 B; the out-of-line
render frames remain 5,120/5,104 B. Static frames do not establish runtime HWM.

This bounds returned frame-render failures. Initial display initialization,
non-returning driver stalls/watchdog resets, lower-level per-transaction recovery,
physical optical/BUSY behavior and faults across reboot are separate. R-04's
result-loss reachability is not closed. HIL should cover persistent/transient
faults, input and sleep during backoff/pause, manual recovery, latest-frame
convergence and power/stack measurements. Device acceptance remains deferred;
no flash, deployment, provider/API calls, push or PR.

Evidence and exact source/SDK/binary identities:
`/home/unknow/projects/doc/robustness-remediation-20261029/r10/`.
