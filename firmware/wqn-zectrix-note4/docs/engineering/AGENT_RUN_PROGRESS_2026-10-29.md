# R-30: bound submitted Agent run observation without replay

Baseline: `2d0964e`, branch `fix/robustness-20261029`.

An accepted POST `/run` could return HTTP 2xx, then produce only read timeouts
forever. The one-second SDK read timeout kept cancellation responsive but did
not bound ownership of the Agent worker, connectivity demand or sleep lease.

`AgentRunProgress` now applies a monotonic observation policy after successful
HTTP setup, only to a submitted run:

| Stream condition | Local observation policy |
|---|---|
| No validated typed event | Detach after 60 seconds |
| Initial accepted/attached receipt or business event | Allow five minutes without further business progress |
| Repeated accepted/attached receipts | Keep the previous progress timestamp |
| Comments, unknown events, invalid JSON/payloads, partial frames | Keep the previous progress timestamp |
| Permission/question awaiting a reply | Suspend the progress deadline |
| Successful reply to the exact pending ID and owning session | Resume with a fresh five-minute budget |
| Failed, stale or wrong-session reply | Keep the pending ask and its suspended deadline |
| GET session observation | Preserve the existing long-lived observation policy |

The pending-ask tracker mirrors the UI's permission and deferred-question slots.
A permission supersedes the old question; an unshown third ask cannot replace
the question the user can actually answer. A permission reply does not release
the pause while its deferred question is still outstanding. Session ownership
uses the existing reply-session normalization, including child sessions.

Parsing now produces one validated typed event for dispatch, progress and
terminal detection. The parser's whitelist remains authoritative. Explicit
cancel retains priority over switch and timeout, including a cancel arriving
during a synchronous reply POST. Failure to install the one-second read timeout
fails the stream setup instead of silently using the longer SDK timeout.

The two timeout codes are `run_first_event_timeout` and `run_progress_timeout`;
the device reports “任务状态未确认，请重新进入任务查看”. A local timeout means the
execution outcome is unknown. It closes the stream without replaying `/run` or
issuing `/interrupt`. The unchanged session-owner failure tail retains the
selected session and logical request ID, releases the worker slot, and drops
both resource holds when no other live-run evidence exists. A known HTTP 409
`run_in_progress` rejection still retires that rejected submission ID and
chains same-session observation, preserving its existing live-run ownership.

The run JSON builder also checks every cJSON field insertion. Allocation
failure must return `ESP_ERR_NO_MEM` before HTTP initialization; it must not
silently omit the request ID that makes a later retry identifiable. Normal
request bytes and public wire contracts are unchanged.

From the application directory:

```sh
python3 scripts/test_agent_run_progress.py
python3 scripts/test_agent_run_progress.py --before 2d0964e
python3 scripts/test_ui_agent.py
sh scripts/run_agent_round_policy_test.sh
python3 scripts/test_agent_disabled.py
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl build
```

The new test compiles the entire production HTTP client with real SSE/cJSON,
ASan/UBSan/LSan and virtual HTTP/time ports. Exact production session-owner
functions are extracted for the cleanup/request-identity checks; RTOS and
resource handles are ports. It passes 164 checks, including 11 run-builder
allocation sites, quiet/trickling streams, long valid work, prolonged human
waits, reply failure/retry, child-session ownership, stale question steps,
deferred/extra asks, cancellation precedence, GET observation, HTTP 409 and
64-bit monotonic uptime. The same harness rejects the baseline with 33 failed
checks; a silent run reaches the scripted virtual EOF after 7,202 seconds
instead of exiting at 60 seconds. This is a bounded reproduction of the old
unbounded read loop, not a measurement of physical wait time.

Five mutation controls independently remove the deadline, ignore user waits,
clear waits on failed replies, ignore reply-session ownership, or treat invalid
frames as progress. All five fail semantic assertions. Existing Agent UI passes
1,764 assertions, round policy passes 1,220 checks, and Agent-disabled coverage
links all 23 public session entry points and passes its refusal/no-op checks.

SDK v5.5.6 builds with zero warnings, M8 ownership and compiled UI stack gates
passing. The actual Xtensa compiler reports a 1,520 to 1,648 byte increase in
the stream entry frame; `RunOpenCodePrompt` remains 208 bytes. The worker has a
9,216 byte stack. These are static entry frames, not complete nested SDK call
chains or runtime high-water marks.

The progress timer runs in the body-read loop. It cannot preempt connection,
header fetch, synchronous reply RPCs or SDK close/cleanup. The initial setup
timeout remains five minutes. A legitimate tool silent for over five minutes
can detach locally while continuing in the cloud; the UI deliberately reports
an unknown outcome and retains the session for observation. This change does
not implement recovery for a provider violating the UI's bounded ask slots.

Physical network stalls, TLS cleanup timing, worker stack high-water marks and
end-to-end device acceptance remain deferred. No model/API calls, deployment,
flash, push or PR were used in this batch. Evidence and exact source/SDK/binary
identities are under
`/home/unknow/projects/doc/robustness-remediation-20261029/r30/`.
