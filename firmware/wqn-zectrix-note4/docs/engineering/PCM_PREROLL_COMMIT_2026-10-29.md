# R-26: commit capture replay with the transport turn

Baseline: `6ba25f8`, branch `fix/robustness-20261029`.

The AI tap used to consume a replay flag armed before `StartTurn`. A capture
block arriving while the owner was still sending `voice.turn.start` consumed
that flag, but both prefix and live pushes were rejected by the transport's
Idle gate. Later live blocks could then reach the model without the prefix.
Moving the arm after `StartTurn` returned would allow the opposite ordering:
the capture task could enqueue live PCM after commit but before the caller ran.

The transport owner now publishes a replay obligation only after an exact full
start-message send and the old PCM queue drain. It initializes sequence, loss
state, replay and parser before publishing `kRecording` with release ordering.
The sole capture producer acquires that state in `PushPcm`; an early call returns
before consuming replay or enqueueing live PCM. Rejected starts cannot arm a
new replay, and every later successful commit initializes its own obligation.

The first admitted capture call snapshots the capture buffer, subtracts the
current block already appended to it, and enqueues two ordered spans: captured
prefix, then current live PCM. Later calls enqueue only live PCM. Both spans
use the existing block-size limit and the same sequence/FIFO. The separate AI
replay flag, exported arm function and caller-side arm were removed, including
the disabled stub and the voice pipe's resulting unused AI-session dependency.

An unavailable/too-short capture snapshot or a failed prefix/live enqueue
latches the existing pre-FINAL loss state and requests owner reset. It cannot
continue with a live-only success. Queue capacity remains 128 blocks (1.92 s
at 240 samples/block); excessive backlog still fails conservatively instead of
claiming lossless audio. No extra audio buffer or heap allocation was added.

Capture may stop after commit but before its first admitted tap. `SendFinalAndWait`
now refuses FINAL while replay is still pending, requests retirement of the
receiving connection, and returns `kDefinitelyNotSent`. The existing shared
voice pipe maps this to `ESP_ERR_INVALID_STATE`; STD and Agent callers retain
the stopped capture and use their existing full-clip batch paths. No FINAL is
enqueued or sent in this branch, preserving the existing definitely-unsent
fallback boundary. A sent or ambiguous FINAL keeps its no-replay semantics.

Run from the application directory:

```sh
python3 scripts/test_pcm_preroll.py
python3 scripts/test_pcm_preroll.py --before 6ba25f8
python3 scripts/test_agent_voice_finalize.py
python3 scripts/test_stream_transport.py
python3 scripts/test_audio_runtime.py
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl build
```

The new ASan/UBSan/LSan test compiles the production tap, public `StartTurn`,
the exact owner start-command branch, `PushPcm` and `SendFinalAndWait` with real
SDK cJSON. RTOS queues/time/notifications, WebSocket I/O and capture snapshots
are controlled ports; the owner dispatch prelude is outside this probe. It
passes 314 checks across 24 valid schedules, including taps before owner
dispatch, inside start send, and immediately after commit before completion
reaches the caller. It checks every sample and sequence for exact ordering,
variable block sizes, owner drain between enqueues, subsequent turns, queue
failures/capacity, invalid snapshots, no-tap FINAL recovery, normal/ambiguous
FINAL, rejected wire sends and 14 start JSON allocation failure sites.

The same harness fails 40 baseline checks. Six mutations independently remove
the commit replay, admit recording before send confirmation, skip the prefix,
continue live after prefix overflow, ignore missing-prefix loss, or permit an
empty FINAL. All six fail semantic assertions. Existing voice finalization
passes 76 checks, transport/HTTP passes 322 WS, 2,844 SSE and 103 buffered HTTP
checks, and audio runtime passes 148 checks. Those tests remain host evidence;
the full-clip callers' batch selection is also confirmed in current source.

SDK v5.5.6 builds with zero warnings; M8 ownership and compiled UI stack gates
pass. Actual Xtensa compiler entry frames are tap 64 to 32 B, `PushPcm` 544 to
560 B, and `SendFinalAndWait` 528 B unchanged. The capture stack remains 6,144 B.
These measurements exclude deeper callees, interrupts and runtime high-water
marks. The queue, WFLV wire format and server contracts are unchanged.

This protocol relies on the existing single capture producer and stopped-worker
join before finalization/new capture. It does not certify arbitrary simultaneous
cross-session capture replacement, physical I2S/DMA continuity, network sample
loss, TLS/queue timing or device scheduling probability. HIL should exercise
commit/tap preemption, immediate release, queue pressure, abort/late completion,
STD/Agent switching and recorded/model prefix comparison. Device acceptance is
deferred by instruction; no flash, deployment, model/API calls, push or PR.

Evidence and source/SDK/binary identities:
`/home/unknow/projects/doc/robustness-remediation-20261029/r26/`.
