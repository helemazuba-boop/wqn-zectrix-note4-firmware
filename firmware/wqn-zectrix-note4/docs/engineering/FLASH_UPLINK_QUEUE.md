# Flash capture and uplink queue

Status: local implementation and host verification. Device sampling continuity,
power, runtime memory peaks and mode/sleep acceptance still require fresh HIL.
No device flashing, deployment or provider invocation is part of the host tests.

## Runtime and protocol

The permanent `flash_stream` task owns per-turn PCM acquisition and never
performs a WebSocket send. It produces L-channel mono s16le at 24kHz in up to
360-sample blocks. A permanent `flash_uplink` task consumes a bounded PSRAM
queue and owns PCM transport. Both tasks block on notifications when idle;
their stacks/TCBs are never reused while a task is exiting.

`g_flash.stream_task` is now an active-capture marker, not the lifetime of
`g_capture_task`. The capture worker clears it only after its last audio/queue
access. Queue producer/sender acknowledgements precede client/channel teardown.
No forced task deletion is used.

The queue contains 192 blocks: 2.88 seconds of full-size PCM (138,240 bytes).
Including generation/count metadata and one unused ring slot, its PSRAM backing
store is 140,504 bytes. The new sender stack reserves another 8,192 PSRAM bytes.
TCBs, controller atomics, pointers and possible library/task allocations also
consume internal RAM. The default ELF accounts for 412 bytes of new static
internal data (348-byte sender TCB, 44-byte queue controller, four pointers and a 4-byte RX counter);
this excludes runtime library/task allocations. Runtime
free/largest/DMA/PSRAM and stack HWM remain device acceptance criteria.

Audio DMA/codec reservation still occurs before playback/WS/TLS allocation.
Queue storage and worker stacks are explicitly allocated from PSRAM. Existing
playback, barge-in local silence + remote cancel, account/token rules and power
leases retain their original owners.

## Turn state and failure behavior

- Begin requires both prior worker acknowledgements. Each block carries its
  immutable generation; stale completion/cancel/data cannot modify a new turn.
- Empty capture sends no FINAL. Normal release waits for capture completion and
  all accepted PCM blocks, with one absolute three-second drain budget.
- FINAL can be claimed once only after the queue is empty and accepted/sent
  counts match. Its transport result must match the complete frame size.
- Overflow, non-timeout I2S errors, malformed stereo bytes, PCM short writes or
  drain timeout fail the entire turn. No FINAL is sent for a partial recording.
  Valid aligned bytes returned with an I2S timeout remain usable PCM.
- An abandoned append requires disconnect before another recording; no silent
  dropping/splicing or automatic utterance/FINAL replay is allowed.
- FINAL delivery failure is an uncertain outcome: the device asks the user to
  check the original result. It does not retry the commit/create sequence.
- Every WS call registers its borrowed client under the Flash mutex, releases
  that mutex before I/O, and releases the registration after I/O. Teardown
  requires both workers idle and all registrations drained. Timeout retains
  the client/audio lease for the existing bounded lifecycle retry.

Default v2 frame encoding stays identical. Legacy v1 still uses per-device
Bearer auth and is compile-only pending server/device acceptance.

## Local verification

Run from the firmware source root:

    python3 scripts/test_flash_uplink.py
    python3 scripts/test_flash_uplink.py --baseline a565923
    python3 scripts/test_recording_indicator.py
    python3 scripts/test_audio_runtime.py
    python3 scripts/test_audio_rx_dma.py
    python3 scripts/test_audio_rx_overflow.py
    python3 scripts/test_flash_auth_headers.py
    python3 scripts/test_voice_endpoint.py

The new fixture compiles production capture/sender/release/teardown functions
and the production queue with ASan/UBSan, for v1 and v2. It checks PCM order,
partial reads, overflow, exact send results, empty/duplicate/aborted FINAL,
pending response, late producer/sender completion, deferred client destruction,
and 40,000 concurrent ordered blocks.

The immutable-baseline comparison uses a fake microphone and one simulated
2.5-second send stall: the old capture loop calls the network 170 times and
takes 5.05 simulated seconds for 170 blocks; the new capture loop takes 2.55
simulated seconds and calls the network zero times. This verifies separation,
not real I2S loss, ASR quality, throughput, battery life or production latency.

## Device acceptance

Use the exact same reviewed ELF/configuration for normal and controlled-network
conditions; record binary SHA, board identity, network treatment and raw timing.
Do not compare historical logs from different builds as a controlled trial.

| Case | Required evidence |
| --- | --- |
| Normal connection, known test audio | Captured samples and known waveform continuity; queue counts/order, zero overflow, response after a complete FINAL. |
| Send stall below queue capacity | Capture remains continuous while queue depth rises; accepted/sent counts converge before FINAL. |
| Sustained stall/overflow | Explicit failure, no FINAL/partial submission, no replay; a fresh connection can recover. |
| Release with pending PCM, empty/fast tap | Exactly one FINAL after complete drain, or no FINAL for empty input; bound drain time. |
| Cancel and disconnect during send/drain/FINAL | Worker acknowledgements, active send count, no premature client/I2S deletion; uncertain FINAL is not retried. |
| Barge-in and five consecutive turns | Local silence + remote cancel retained; no prior PCM or TTS enters the next generation. |
| Flash → STD/PRO/Agent, sleep and wake | Old owners quiesce before reuse; no stale access, allocation failure or lost input. |
| Cold and repeated sessions | Internal/DMA/PSRAM free/largest/peaks and both worker HWMs; retained PSRAM allocation appears once and does not grow per turn. |

The capture-stop, queue-drain and FINAL-transport logs carry the turn generation.
Drain logs include worker HWM plus internal/DMA/PSRAM free and largest blocks.
AudioService registers an IRAM RX-queue overflow callback before enabling duplex
channels. Its counter stays in internal DRAM; the per-turn log subtracts the
post-warmup baseline from the capture-stop value. Registration failure follows
the existing two-channel rollback. The S32C1I workaround prevents a compile-time
lock-free assumption; verify the callback and atomic helper IRAM placement in
the ELF. Host callback/rollback tests cannot establish ISR latency.

These are point-in-time snapshots, not runtime memory peaks; transport success
is not a server receipt.

Fresh RX-overflow/known-waveform evidence is still required. Overflow events
measure overwritten driver queue buffers, not an exact microphone loss rate. Sample-count/wall
ratios alone do not measure hardware loss. Do not claim that host success or a
zero-warning build completes HIL. The application plan's no-auto-flash boundary
remains in force until the device operation is explicitly authorized.

Rollback is a coherent capture/sender/queue change; do not remove SHA,
capability/account checks, playback cancellation or audio resource ownership.
