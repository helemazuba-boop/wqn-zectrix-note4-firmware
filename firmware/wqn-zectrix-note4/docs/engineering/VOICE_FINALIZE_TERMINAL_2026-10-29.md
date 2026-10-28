# R-22: observe transport termination while finalizing ASR

Baseline: `206726c`, branch `fix/robustness-20261029`.

After FINAL was confirmed sent, the shared STD/Agent ASR pipe only polled its
ASR result flags. A matching transport terminal without an ASR/error callback
was ignored until the caller's whole budget elapsed (15 s Agent / 120 s STD).
The transport owner already recorded the terminal; the worker could not observe
it without entering the separate blocking release wait.

`TryGetTurnTerminal` now copies a coherent terminal snapshot under a short
critical section and validates both generation and request ID. It does not
consume the record/semaphore, wait, disconnect or mutate the turn. The publisher
uses the same protection; logging and semaphore operations remain outside it.
`WaitForTurnRelease` also reads the sticky snapshot before waiting and rechecks
after a timeout, so an already-consumed signal cannot hide a matching record.

Finalize polls accepted ASR results, cancellation and the identity-bound terminal
at a 100 ms slice. A matching terminal with no ASR result returns
`ESP_ERR_INVALID_RESPONSE`, without another release wait or HTTP fallback. A
result committed while the terminal snapshot is being taken wins after a second
flag read. Only `kDefinitelyNotSent` still authorizes the batch path; sent and
ambiguous FINAL are never automatically replayed. A live silent turn retains its
deadline and bounded release-timeout recovery. Positive sub-tick remainders wait
at least one RTOS tick rather than spinning on zero-tick polls.

ASR complete/error flags are now published after the caller's observer returns.
Previously the trampoline forwarded before giving the semaphore, but set the
polled flags before forwarding; that did not ensure the observer's message was
ready for a polling worker. The post-observer publication is bound to the same
active request and generation, so an aborted/changed turn cannot receive the
late result flag.

Validation:

```sh
python3 scripts/test_agent_voice_finalize.py
python3 scripts/test_stream_transport.py
python3 scripts/test_std_ai.py
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl build
```

The new test compiles the complete real voice pipe, real headers and the actual
terminal publisher/query/release-wait bodies. SDK clock/semaphores/transport
commands are host ports. At virtual 100 Hz, a terminal at 100 ms now returns at
100 ms for both 15 s and 120 s budgets; the baseline exhausts those budgets.
All 76 checks pass under ASan/UBSan with leak detection, including old-identity
rejection, already-published terminals, same-slice ASR/terminal, observer ordering,
ASR publication during the query, cancellation, ambiguous send, definitely-unsent
fallback and deadline recovery. A concurrent reader observes 20,000 publications
using the real ledger bodies with a host mutex critical-section port; this
checks coherent snapshots, not ESP32 interrupt timing. Three mutation controls
remove generation matching, request matching or the post-snapshot ASR re-read;
the tests reject each mutation. A separate intermediate-source check reproduced
the observer-publication race before that correction.

Existing HTTP/SSE tests pass 2,844 checks, WS consumer/FINAL teardown passes 232,
and STD session behavior passes 31 assertions. ESP-IDF v5.5.6 builds with zero
warnings, M8 ownership and compiled UI stack gates passing. Device scheduling,
actual disconnect latency and physical/network HIL remain unverified.

This addresses R-22's specific worker wait. It does not close the historical STD
read/close/cleanup incident whose old R-02 mechanism was withdrawn. R-24's
ASR-complete-but-missing-release owner lifetime remains separate: successful ASR
still returns without blocking for release. No flash, deploy, production traffic,
push or PR occurs in this batch. Evidence:
`/home/unknow/projects/doc/robustness-remediation-20261029/r22/`.
