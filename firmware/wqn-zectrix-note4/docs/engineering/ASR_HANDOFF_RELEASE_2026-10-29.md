# R-24: bound ASR handoff ownership when release is missing

Baseline: `2896a71`, branch `fix/robustness-20261029`. The frozen audit's
classification remains partially confirmed; this batch repairs its specific
ASR-result/missing-release window and the related detached soft-cancel path.

Previously, the real voice pipe returned a completed transcript immediately.
Without a matching `turn.released`, a healthy connection could remain in
`RemoteHandoff` indefinitely. Both the next `StartTurn` and idle disconnect
require an idle turn. The ordinary no-result path already had finalize timeout
then release observation/disconnect; that existing recovery was not missing.

The transport owner now owns a fixed-size, allocation-free deadline bound to
both committed generation and request ID:

| Condition | Owner policy |
|---|---|
| Exact successful FINAL send for tier `agent` (ASR-only) | Arm 150-second absolute handoff cap |
| ASR-only `asr.complete`, `asr.failed` or `error` | Shorten to three seconds after callback returns |
| `turn.done` / decoded `response.done` | Shorten to three seconds for any tier |
| Explicit foreign request ID | Do not arm the current turn's short deadline |
| Legacy business terminal with no request ID | Bind the deadline to the current owner's identity |
| Repeated terminals, comments or other data | Never extend an existing deadline |
| Matching nonempty-ID `turn.released` | Publish Released, clear the deadline and reuse the ready connection |
| Expired current identity in RemoteHandoff | Publish LocallyAbandoned and run existing stop/destroy cleanup |
| New committed turn or teardown | Reset the deadline |

Tier `agent` is the existing ASR-only relay route used by the shared voice
pipe. Generic `std`/`pro` full dialogue may continue generating text/tools
after ASR: ASR completion does not start the short deadline for those tiers,
and their handoff does not receive the ASR-only 150-second cap. Their decoded
conversation terminal starts the release grace instead. This does not add a
general full-dialogue inactivity deadline.

The 150-second cap exceeds current callers' 120-second ASR wait plus release
observation and also applies after a soft cancel has returned without waiting
for any result. A late business terminal can shorten, never renew, this cap.
An ASR result is delivered before the deadline is armed; finalize still returns
the recognized text immediately. The cleanup neither submits audio again nor
sends a second FINAL or remote abort. LocallyAbandoned describes local ownership
ending with an unknown remote release outcome. It preserves the last actually
observed business stream terminal; it does not pretend the server released.

Cloud source inspected at `91ce1f22620c470838288fba8cb69ace77e8680f` in
`/home/unknow/projects/.worktrees/WQN-flash-20261028`:
`web/server/realtime-proxy/src/batchVoiceRelay.ts` selects ASR-only for `agent`
and forwards backend SSE; `web/lib/sse-pipeline-types.ts` emits ASR completion
without a request ID, and `web/lib/ai-stream.ts` does not inject one. Its
release-finally is identity guarded but cannot guarantee delivery. No cloud
wire changes are needed here. Supporting legacy empty-ID business terminals
does not claim to solve all stale-event routing; R-25 remains separate.

From the application directory:

```sh
python3 scripts/test_ws_release_deadline.py --tick-hz 100
python3 scripts/test_ws_release_deadline.py --tick-hz 1000
python3 scripts/test_ws_release_deadline.py --before 2896a71
python3 scripts/test_stream_transport.py
python3 scripts/test_pcm_preroll.py
python3 scripts/test_agent_voice_finalize.py
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl build
```

The new ASan/UBSan/LSan test compiles the complete production voice pipe, the
complete transport owner root loop, actual turn-start case and public APIs,
SSE consumer/decoder, cJSON, WFLV encoder, teardown and terminal ledger.
Connect/RTOS/SDK I/O are virtual ports. The synchronous control notification
port executes the actual start case; it does not reproduce task concurrency.
It passes 166 checks at each tick rate, including:

- ASR at 100 ms returns immediately, missing release is cleaned up at the
  first owner poll after the 3,100 ms due time, and the next pipe turn commits.
- Fragmented SSE (1/17/512-byte reads), matching release exactly at deadline,
  reuse without disconnect, stale release, foreign and legacy terminal IDs.
- Duplicate terminals and keepalives do not renew grace; local abandonment
  retains observed `turn.done`; errors and slow ASR preserve result behavior.
- Full dialogue continues beyond 150 seconds after ASR; its final business
  terminal starts the short release grace.
- Soft cancellation without any result/release reaches the 150-second cap;
  a terminal near that cap does not extend it. The ordinary no-result finalize
  timeout still uses its existing release observation/disconnect.
- Generation/request identity, reset, invalid identities/budgets, reused ID
  with a new generation, and 64-bit monotonic uptime.

The same fixture compiles the baseline and rejects it with 35 failed semantic
checks within a finite observation window. Seven mutations (no expiry,
generation/request mismatch ignored, duplicate renewal, applying ASR grace to
full dialogue, short grace from FINAL, and absent absolute cap) are rejected
by semantic assertions at both tick rates. Compilation/harness errors are not
accepted as mutation evidence. Existing tests retain 314 PCM-preroll, 76
voice-finalize, 322 WS, 2,844 HTTP-SSE and 103 buffered-HTTP passing checks.

SDK v5.5.6 builds with zero warnings; M8 ownership and compiled UI stack gates
pass. The timer is checked by the existing owner loop (normally polling every
50 ms) after notification and control processing. It cannot preempt a blocking
callback, SDK send, stop/destroy or other stalled owner operation. The stated
deadlines initiate the existing local cleanup; they are not a hard bound on
completion of SDK cleanup. There is no force-destroy of a running task.

Device network/cleanup timing, callback concurrency, runtime stack high-water
marks and end-to-end acceptance remain deferred. Historical STD read/close/
cleanup field failures are not closed by this batch. No API/model calls,
deployment, flash, push or PR were used. Exact source/SDK/binary identities,
baseline/mutation logs and regression evidence are under
`/home/unknow/projects/doc/robustness-remediation-20261029/r24/`.
