# R-32: retain AI full-waveform intent through final display submission

Baseline: `04266a9`, branch `fix/robustness-20261029`.

`CopyAiStreamingStatus` transfers and clears a one-shot `force_full_render`.
The runtime previously converted it only into an AI refresh schedule and a new
revision. Neither carries a waveform requirement: final dispatch could select
Auto, or suppress the whole request when the pixels matched the presented or
submitted signature. A later periodic panel cleanup did not fulfill that event.

`UiUpdate` now carries `force_full_refresh` independently from state changes and
drawing cadence. `DispatchAiStreamingSnapshot` sets it when the AI screen is
visible and the observation requires a full refresh. A pure waveform repair
still advances the display revision without inventing a pixel-state change.

The UI owner ORs this requirement into its existing `force_next_submission`
state. The final dispatch already uses that state to prefer a full frame,
request `WaveformRequirement::kFull`, and bypass identical-pixel deduplication.
It retains the requirement and refresh schedule through stream throttling or
request rejection, then clears it after the accepted intent is recorded. An
ordinary later snapshot cannot erase a deferred requirement.

Once accepted, the existing display pipeline owns that obligation: policy
merging preserves Full while allowing newer pixels to supersede older pixels.
Full waveform is normalized to the existing commit rendering path there.
Presented/Failed acknowledgements retain their existing behavior. This batch
does not change the cross-intent retry budget/backoff tracked by R-10.

The visible-screen condition is unchanged. An AI force observation received
while another page is visible does not invent a refresh for that page. If a
visible AI repair has already been deferred and the user navigates, its full
waveform applies to the newest page rather than resurrecting stale AI pixels.
The drawing schedule still follows normal coalescing, separate from waveform.

From the application directory:

```sh
python3 scripts/test_ai_force_full.py
python3 scripts/test_ai_force_full.py --before 04266a9
python3 scripts/test_ui_display_submit.py
python3 scripts/test_ai_reading.py
python3 scripts/test_ui_agent.py
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl build
```

The ASan/UBSan/LSan probe compiles the production snapshot transfer, exact typed
update, reducer/revision logic, real owner handoff block, complete final dispatch
lambda, waveform selector, display tracking and result ACK handling. Rendering,
frame signatures, request acceptance, RTOS/time and panel I/O are ports. The
signature port deliberately holds pixels identical; production signature code
also does not include `prefer_full_refresh`. This is delivery-policy evidence,
not a physical panel or complete `DeviceUiTask` test.

All 57 checks pass: same presented/in-flight pixels, fresh repair revisions,
single consumption, rejected-request retry, STD/Agent/Flash throttle retention,
navigation during deferral, hidden-screen and ordinary Auto/dedup controls,
legacy/global and commit force, schedule coalescing, and Presented/Failed ACKs.
The same test fails 37 baseline assertions. Six mutations independently drop
the reducer bit, drop the owner handoff, overwrite an already pending bit,
clear it on throttle or rejection, or ignore it at final dispatch. All fail
semantic assertions without compiler or sanitizer errors.

Existing display-submit/merge passes 96 checks, AI reading passes 24,483
assertions and Agent/STD UI passes 1,764. SDK v5.5.6 builds with zero warnings;
M8 ownership and compiled UI stack gates pass. The compiled UI owner frame is
1,872 B; its two out-of-line render frames remain 5,120 / 5,104 B. These are
static entry frames, not nested call-chain or device stack high-water proof.

Physical ghosting removal, actual waveform execution and button/refresh timing
still require HIL. Device acceptance is deferred by instruction. No provider
calls, flash, deployment, push or PR. Evidence and source/SDK/binary identities:
`/home/unknow/projects/doc/robustness-remediation-20261029/r32/`.
