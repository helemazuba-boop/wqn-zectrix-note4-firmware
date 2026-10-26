# STD AI interaction

STD now reuses Agent decision cards while retaining its own model/context and
backend. The shared ASR pipe is compiled with AI, independently of Agent enable.

## User flow

- Hold Confirm for 200 ms to capture, release to transcribe. Partial recognition
  appears in the pending card. The recording LED still follows actual capture.
- Confirm the transcript with 发送, or discard it with 重说. No unconfirmed input
  is appended to chat history or sent to the model.
- `ask_question` lets the STD model ask one bounded question with up to eight
  choices. Up/Down walks the same two-slot window as Agent. 自己回答 interrupts
  this turn so the user can record a different answer; it does not edit text.
- Todo creation/status writes and new Note writes show their concrete parameters
  and wait for 执行/取消. The card uses up to twelve preview lines; a longer
  preview ends with an ellipsis. Read tools run immediately.
- A modal decision consumes all physical key edges. It cannot accidentally start
  PTT, enter the top bar, or scroll the underlying answer. A new card resets focus
  and requires a fresh physical press after it was armed; queued old keys cannot
  approve an unseen operation.

## Ownership and recovery

The single STD worker owns capture/transcription and confirmed text submission.
It drains an identity-bound reply mailbox between HTTP stream reads, woken by
500 ms server heartbeats. UI and SSE callbacks never issue HTTP. Lost reply
responses keep the original option and interaction ID; changing an uncertain
answer is refused. The server's closed event also retires a matching card.
Leaving STD cancels pending input/capture and requests cancellation of the stream;
late results cannot answer a newer question. Once a write has started, the cloud
finishes its existing transaction/receipt protocol rather than creating another
operation. A temporary unknown result must not be treated as a failed create.

There are no new tasks, persistent footer hints, EPD owner calls, GPIO/waveform,
sleep policy or firmware storage-protocol changes.

## Coordinated cloud dependency

Deploy the matching WQN routes and apply
`20261026000000_esp32_ai_interactions.sql` before device acceptance. The protocol
is documented in `contracts/ai-interaction-v1/README.md`; its schema digest is
pinned in `main/ai_interaction.h`. The operation payload binds user/device,
request/call IDs, exact arguments and the original time context. Approval is
followed by the existing database permission and ownership checks.

## Local verification and device acceptance

Host regression entrypoints:

- `python3 scripts/test_std_ai.py` — production input and reply gates, rejected
  duplicate confirmation, cancellation, ASR-only fallback, reply identity/retry.
- `python3 scripts/test_ui_agent.py --render-dir /tmp/wqn-std-ui-preview` — actual
  shared decision drawing, question window, modal input and original Agent UI.
- `python3 scripts/test_stream_transport.py` — production HTTP/WS owners and
  decoder, confirmed JSON input, ASR-only routing, reply pump and cancellation.
- `python3 scripts/test_ui_navigation.py` — screen transitions and modal vetoes.
- Agent-on/off ESP-IDF builds, zero warnings, M8 and compiled UI entry-frame gates.

Host rendering, ASan/UBSan and builds are not device HIL. Device acceptance must
cover microphone start/release/cancel, input send/discard, multi-option/custom
answers, Todo/Note approve/reject, weak network during approval, repeat taps,
page/tier departure, physical EPD latency and combined HTTP/TLS stack/heap peaks.
No cloud deployment, production migration or flashing is part of local commits.

### Local evidence, 2026-10-26

- WQN: 181 test files / 1,753 tests passed, with four files / eleven tests
  skipped by their existing opt-in conditions. The isolated PostgreSQL run
  separately passed all ten interaction and durable-write tests against a fresh
  disposable database with this migration applied.
- Production Webpack build, TypeScript, and client-bundle secret scan passed
  with the repository CI's public placeholder environment. The default
  Turbopack build encountered this environment's port-bind permission denial;
  it is not recorded as a passing Turbopack build.
- Firmware: 31 STD session assertions, 1,764 Agent/STD UI assertions, 1,492
  navigation assertions, 2,844 HTTP checks and 232 WS checks passed. Reading
  regression passed 24,432 assertions with sanitizers; the Agent-disabled host
  test linked all 22 public entry points.
- `build-wsl` and the independent Agent-disabled `build-agent-off` both built
  with zero compiler warnings, M8 ownership and compiled UI stack gates passed.
  The disabled profile used a separate SDKCONFIG; the generated headers were
  checked to contain Agent only in the enabled build.
- During an earlier denied-function permission probe, the local Supabase
  PostgreSQL image crashed once with signal 11 and recovered automatically.
  The final privilege assertions inspect actual function/table ACLs instead;
  successful application tests do not validate that image's denied-call error
  handling. No production database was used.

The device acceptance items above remain pending.

## Streaming display parity, 2026-10-26

STD keeps its existing confirmed-text SSE route and model. Its thinking deltas
now update the live history throughout generation, with a new entry for each
post-tool reasoning round. Text and tool blocks retain their original order;
a cumulative final reply cannot duplicate already sealed pre-tool narration.
Terminal/error frames flush the last coalesced prefix. The existing UTF-8 text
budgets remain in force.

STD and Agent now share the same viewport behavior: watch thinking/tool stages,
park at the beginning of each answer while it grows, continue through later
rounds, and stop at completion or a manual reading gesture. Capture, input
confirmation and pending decisions pause movement. Automatic viewport writes
check the owner's current epoch and phase, and respect the disabled-follow
setting. Completion no longer resets STD to the top of the question.

Both modes sample live display frames at the same 500 ms floor. This changes UI
coalescing only; the display owner, waveform, rail and sleep policies are intact.
The cloud already emits thinking/text/tool deltas; this follow-up changes no wire
schema, provider API or database migration.

Additional validation entrypoints:

- `python3 scripts/test_std_stream.py` — actual STD SSE consumer and real
  `AiHistory`, with ASan/UBSan. Covers live thinking, multiple model/tool rounds,
  final narration deduplication, event order, coalesced terminal flushes and caps.
- `python3 scripts/test_ai_reading.py --sanitize` — actual STD/Agent viewport
  dispatch and epoch-checked owners with real layout geometry. Covers variable
  UI cadence, resume over old content, manual reading, modal pauses and disabled
  following, alongside the existing pixel/cache/reading regressions.

Device acceptance must additionally cover a preface followed by several tools,
question/write decisions between model rounds, long thinking/body generation,
manual paging during execution and the final answer's physical panel position.
Host replays and builds do not complete those device checks.

Follow-up local results: reading/layout/follow **24,472** assertions; STD
SSE/history **26**; STD input/decision gates **31**; Agent/STD UI **1,764**;
navigation **1,492**; HTTP **2,844** / WS **232**; audio runtime **90**;
recording indicator **67**. The Agent-disabled host link covered **23** entry
points. Each batch built successfully with zero compiler warnings, M8 and UI
entry-frame gates; the final enabled and independently disabled Agent profiles
both passed. Device HIL remains pending. The unrelated Flash/audio working-tree
changes were excluded from these commits.
