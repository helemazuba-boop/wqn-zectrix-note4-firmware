# R-07: bound storage preparation without abandoning caller stack data

Baseline: `3068868`, branch `fix/robustness-20261029`. The frozen finding remains
partially confirmed. Ordinary writes already hold kStorage and make sleep
preparation refuse admission; the repaired window is maintenance submitted
after that admission, with no existing blocker.

Previously `PrepareWordObservationOutboxForSleep` placed maintenance context on
the PowerCoordinator stack and dispatched background transactions. Both queue
admission and completion used portMAX_DELAY. An in-step deadline could defer
the next checkpoint but could not free a caller stuck waiting for the owner.
Simply timing out those waits would let the owner access expired stack data.

Sleep preparation now submits an independently owned maintenance request to
the same storage owner, retaining all seven mode checkpoints and the final
proof-checked compaction. The caller observes one finite absolute esp_timer
deadline across queue admission and completion; the second wait recomputes
the remaining budget. Finite waits round up to the next RTOS tick and never
become portMAX_DELAY. A late success signal is reported as timeout, not Ready.

The new `ExecuteOwnedStorageTransactionUntil` API consumes the independent
payload when a cleanup function is supplied, including on setup failure. It
is refused on the storage task itself rather than running inline without a
caller deadline. Every referenced input and resource hold must be owned by
the request; the diagnostic owner string must have static lifetime. Existing
foreground/background APIs keep their caller-owned context and running-wait
semantics; arbitrary stack-context transactions cannot use the new detach path.

| State at the caller deadline | Result and ownership |
|---|---|
| Not enqueued | Cleanup locally; return timeout |
| Queued, caller wins Queued→Abandoned | Return timeout; queued command retains payload/completion until owner skips and reaps it |
| Owner already claimed Running | Return timeout; owner continues with its independent context |
| Completed before deadline | Return actual result; final reference reclaims payload/completion |
| Completed at/after deadline | Return timeout even if the observed transaction result was success |

There are separate caller and command references to the completion. The command
reference survives through the whole semaphore-Give operation, so a caller
receiving that signal cannot destroy semaphore storage still being used by the
publisher. Result publication precedes the terminal state/signal. A worker
claiming a request whose deadline already passed does not invoke its transaction.

`SleepLease::TryAcquireStoragePreparation` is restricted to kStorage and the
current nonzero quiesce generation, checked again under the lease registry lock.
Ordinary lease admission remains closed during quiesce. Power broadcast now
passes its actual generation through the storage boundary. The request owns
this real storage/NO_LIGHT_SLEEP hold through queue and running timeout.

After a normal sleep abort reopens admission, a detached request still blocks
the next normal quiesce. An already queued abandoned request conservatively
keeps the hold until the owner reaps it. A running maintenance request releases
the hold only after all of its VFS work has returned, before publishing Ready;
its cleanup also releases the hold on all unexecuted/error paths. The journal
and checkpoint proof are not copied back through a caller stack pointer.

Maintenance rechecks its deadline after cache loading and before initiating
the checkpoint/reclaim call. An existing VFS call remains non-preemptive. Once
it returns after timeout, the request stops before beginning a further step.
Checkpoint-before-reclaim ordering, live-cache rewrite and existing mutation/
scope/ACK proof invalidation remain unchanged. Sleep maintenance runs as one
owned transaction whose existing nested dispatches execute inline on the sole
owner; during healthy quiesce foreground admission is closed. After rollback,
an in-flight step may finish before foreground work resumes, as before.

From the application directory:

```sh
python3 scripts/test_storage_prepare_deadline.py --tick-hz 100
python3 scripts/test_storage_prepare_deadline.py --tick-hz 1000
python3 scripts/test_storage_prepare_deadline.py --before 3068868
python3 scripts/test_word_session_replay.py
python3 scripts/test_audio_runtime.py
python3 scripts/test_binary_outbox_recovery.py
python3 scripts/test_storage_evidence.py
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl build
```

The new ASan/UBSan/LSan fixture compiles the complete production storage service
and sleep coordinator, plus actual maintenance context/step/driver, owned
sleep request, public storage boundary, power broadcast, per-service budgets
and Ready gate. The owner runs on a real host thread. RTOS queues/semaphores,
virtual esp_timer, PM locks, cached record types/allocator and VFS calls are
ports; checkpoint barriers control the interleaving. A test-only observation
after the real request delete counts completed cleanup. This is not an ESP32
scheduler, physical VFS or power-loss timing test.

It passes 108 checks at each tick rate. It covers a stalled cache load,
checkpoint and reclaim; queue admission consuming 90% of the budget; abandoned
commands skipped; request/completion allocation and semaphore/admission failures;
actual lease counts and PM holds after timeout; stale/zero/rolled-back generations;
recovery after owner completion; expiry before dispatch or during owner CPU-lock
setup; I/O errors; and a caller
returning while the publisher is deliberately held inside semaphore Give.

Actual power-broadcast tests retain all seven checkpoints/reclaim on the healthy
path. With a stalled storage checkpoint, storage is TimedOut at its five-second
budget, later audio/connectivity prepares still run, and the real normal-sleep
Ready gate rejects the round. Generation-bound request ownership is exercised
through that actual call path, rather than assuming direct API use proves it.

The baseline compiles and fails its shared running-checkpoint deadline assertion:
a one-second virtual budget is exceeded by one second before the test releases
the stalled owner. This finite observation does not assert that the original
portMAX_DELAY waiter would terminate without that release. Eight mutations at
both tick rates remove remaining-budget accounting, free payload too soon,
drop the blocker, ignore generation, execute expired requests, accept late Ready,
leak the command reference, or retain only the initial step deadline check.
Six fail semantic assertions; premature deletion triggers ASan use-after-free
and the lost reference triggers LeakSanitizer. Compilation/harness errors are
not accepted as rejection evidence.

Existing replay/checkpoint coverage passes 195 checks, audio/storage runtime
148, Word binary-outbox recovery 540, Note recovery 590, and storage-evidence
analysis 23 tests. SDK v5.5.6 builds with zero warnings; M8 ownership and compiled
UI stack gates pass. Source, SDK/config and bin/ELF SHA256 identities and logs
are archived under
`/home/unknow/projects/doc/robustness-remediation-20261029/r07/`.

This bounds the power caller's queue/completion waits, not a running SPIFFS/NVS
operation, allocation/cleanup latency or a task that no longer gets CPU time.
A permanently stalled owner can retain the owned request and blocker until
owner recovery or reset; this patch does not restart a writer or force-destroy
its task. A timed-out result is never permission to replay a write. The existing
battery-emergency/user-shutdown policy can continue after service failure and
cut power; this patch does not change that policy or claim all shutdown modes
wait for every write. Actual flash power-loss safety, emergency reserve energy,
FreeRTOS timing/concurrency and stack high-water marks remain deferred HIL.
Ordinary C++ vector-allocation failure is not covered by the two nothrow-request
allocation checks. No device/model/API calls, deploy, flash, push or PR occurred.
