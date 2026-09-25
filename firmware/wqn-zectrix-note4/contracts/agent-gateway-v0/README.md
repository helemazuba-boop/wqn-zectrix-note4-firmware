# WQN Agent Gateway v0 baseline

This contract freezes the wire vocabulary between the Note4 firmware's **Agent
tier** and the WQN cloud's `/api/esp32/agent/*` routes, which in turn proxy a
self-hosted [OpenCode](https://opencode.ai) server bound to the user's account.

The Agent tier replaced the retired **Pro** tier in the AI page. Pro was a
cloud-side model swap behind the exact same endpoint, prompts and
reasoning-effort mapping as STD, selected only by an `X-WQN-Ai-Tier: pro`
header no other client ever sent. Agent is a different shape: the model, the
tools and the permission policy are all chosen server-side by the binding, and
the device's job is to render a live stream and answer permission asks.

`manifest.json` carries a `version` field for this contract even though its
three sibling contracts (`word-study-v1`, `problem-study-v1`,
`device-control-v3`) do not: the Agent gateway is the contract that an upstream
major version can change under us, so it needs a comparable anchor. A change to
the device-visible surface bumps it; a cloud-only change does not.

## Ownership

| Concern | Owner |
| --- | --- |
| Model choice, tool execution, secrets | Gateway (WQN cloud → bound OpenCode server) |
| Session lifetime, permission policy | Gateway |
| Device pairing token | Device (never a provider key) |
| Rendering, turn navigation, local history | Device |

The firmware never talks to the OpenCode server directly and never holds a
provider credential. It authenticates to the cloud with the same
`Authorization: Bearer <token>` pairing token the rest of `/api/esp32/*` uses,
so the security boundary is unchanged: **HTTP 401 is the only condition that
clears the stored token**; timeout, 429 and 5xx keep identity and surface the
gateway's `error.code` / `error.message` to the user.

## Transport

All paths are relative to the ESP32 API base (`WQN_API_BASE`, default
`https://wqn.helema.cn/api/esp32`).

| Method | Path | Body | Purpose |
| --- | --- | --- | --- |
| `GET` | `/agent/sessions` | — | List bound sessions (device shows at most 12) |
| `POST` | `/agent/sessions` | `{}` | Create a new session |
| `POST` | `/agent/transcribe` | PCM16 LE | ASR for a held PTT recording |
| `POST` | `/agent/sessions/{id}/run` | `{text, confirmed}` | Start or resume a run, stream its events |
| `GET` | `/agent/sessions/{id}/events` | — | Observe a run started elsewhere |
| `POST` | `/agent/sessions/{id}/permission` | `{permission_id, decision, confirmed}` | Answer a pending ask |
| `GET` | `/agent/sessions/{id}/history` | — | Backfill a transcript this device has not rendered |
| `POST` | `/agent/sessions/{id}/question` | `{question_id, answer, confirmed}` | Answer a form ask with the chosen option value |
| `POST` | `/agent/sessions/{id}/interrupt` | — | Stop a run that was already submitted |

Every response is a `{ "data": … }` envelope; every failure is
`{ "error": { "code", "message" } }`. The two streaming endpoints answer
`Accept: text/event-stream` and speak the SSE dialect already used by the
v2-streaming AI tier (`event:` + `data:` frames, one JSON object per data
payload).

`/agent/transcribe` sends raw PCM with the audio shape in headers rather than
multipart, matching the existing AI audio endpoints:
`X-WQN-Audio-Sample-Rate: 16000`, `X-WQN-Audio-Sample-Format: s16le`,
`X-WQN-Audio-Channels: 1`, `X-WQN-Audio-Duration-Ms: <capture ms>`.

### Session identity

A session id is opaque to the device but must start with `ses_`. The device
uses that prefix as a cheap validity check and **silently drops** rows that
fail it — a malformed list is never an error the user sees, it just yields
fewer rows. `POST /agent/sessions` returning anything without the prefix is
treated as `invalid_response`, because the device has just created something it
cannot address.

## Stream vocabulary

A run stream is a sequence of SSE frames. `agent.accepted` and
`agent.attached` are acknowledgements with no payload the device reads. The
remaining nine carry data:

| Event | Data fields | Device effect |
| --- | --- | --- |
| `agent.status` | `status`, `message` | `idle` ends the stream and completes the run; `error` ends it failed; `retry` is reported as a retry; anything else is "executing" |
| `agent.text.delta` | `delta` | Appends to the live assistant block |
| `agent.text` | `text` | Replaces the live assistant block (repair frames only) |
| `agent.reasoning.delta` | `delta` | Appends to the thinking block, **never** to the answer |
| `agent.reasoning` | `text` | Replaces the thinking block in place (repair frames only) |
| `agent.tool` | `tool`, `call_id`, `status`, `preview` | One history block per call, coalesced by `call_id` (falling back to the name when the gateway could not learn it) |
| `agent.permission` | `permission_id`, `type`, `title`, `preview` | Enters `kAwaitingPermission`; the ask is answered by a separate POST |
| `agent.question` | `question_id`, `title`, `options[]` | Enters `kAwaitingQuestion`; the ask is answered by a separate POST |
| `agent.error` | `message`, `fatal` | `fatal: false` records the message and keeps running; absent or `true` fails the run and surfaces the message |

**Termination.** The device reads until it sees `agent.status` with
`status: "idle"`. A stream that ends for any other reason — socket close,
timeout, upstream drop — is `stream_incomplete`, not a success: a run the user
believes finished but did not is worse than a visible failure. The stream has
a 5-minute socket timeout and the gateway is expected to keep it alive well
inside that.

**Reasoning is its own channel.** `agent.reasoning.delta` accumulates into a
buffer separate from the answer and `agent.reasoning` replaces that block in
place, so neither can fold chain-of-thought into `agent.text*`. The upstream
migration made this structural rather than a filter: v2 emits reasoning as a
separate event family, and the gateway projects it onto this separate pair.
The device-side consequence is that a gateway mapping mistake degrades into a
missing thinking block rather than a wrong answer.

**`agent.text` / `agent.reasoning` are repair frames.** They are only emitted
when deltas were lost, so a correct stream sends deltas only. The device always
replaces rather than appends, which is safe in either case.

**Asks and replies are out-of-band and one at a time.** While a stream is open
the device POSTs the reply on a short-lived second connection between stream
reads, then drains the queue again. It never opens a second task or a second
long-lived TLS session. The device holds **one** pending permission and **one**
pending question, so the gateway is expected to hold a second ask back until
the first is answered. If that POST fails, the pending ask is restored so it can
be retried rather than leaving the run blocked behind a silent failure — the
device does not assume the ask was answered. A pending ask is cleared only by a
terminal status (`idle` or `error`), never by an intermediate one.

The device defends against a gateway that does *not* hold the second ask back: a
question arriving while the option bar is taken is **deferred**, not dropped —
the gateway has already marked it seen and will not re-send it, so dropping it
would block the run behind a prompt the user never saw. The deferred ask takes
the bar the moment the permission is answered. Only one can be held, because the
option bar has one mode; a third concurrent ask is logged and lost.

**A question is answered by option value, never by field id.** The gateway
projects the upstream form onto at most two `{value, label}` options and is the
only component that knows which upstream field they came from; the device sends
the value it rendered and the cloud assembles the answer record. A form that
cannot be projected into two options is not armed on the device at all — it is
delivered as `agent.status {status: "busy"}` telling the user to answer in
OpenCode, because an unanswerable prompt is worse than a clear instruction.

**Interrupt is a success even when there was nothing to interrupt.**
`POST /agent/sessions/{id}/interrupt` answers `{data:{interrupted}}`, and
`interrupted: false` means "that run had already finished", which is not an
error. The device makes the POST from the stream worker itself: the UI thread
never opens a connection while a stream is attached.

## What the device does *not* mirror into history

A pending permission or question is rendered as a live overlay (dashed bubble +
option bar) and is deliberately not written to the local history. Its outcome
already appears as the tool blocks and text that follow, so mirroring it would
show the same decision twice.

## Bounds

These are device-side limits, not protocol requirements. The gateway should
stay well inside them.

| Bound | Value |
| --- | --- |
| Sessions listed | 12 |
| Assistant text per run | 12 KiB |
| Prompt per run | 4 KiB |
| JSON body | 16 KiB |
| SSE frame (single line, and accumulated payload) | 16 KiB |
| Stream socket timeout | 5 min |

`GET /agent/sessions/{id}/history` must fit the same 16 KiB JSON ceiling as any
other response, so the gateway is what trims: at most 24 messages, oldest
first, with each text or thinking field capped at 2 KiB and at most 8 tools per
message. A history response the device cannot accept is `invalid_response`, not
a truncated render — the device would rather retry than show half a transcript
and call it the whole one.

## Local history channels

Each tier owns its own `AiHistory` channel so a late background stream cannot
write into whichever tier happens to be visible. The Agent tier uses
`kAgent`; `kStdPro` and `kFlash` are unchanged. The channel is selected
explicitly at every call site rather than inferred from the current tier.

## Validation status

`fixtures/` holds the golden artifacts shared with the WQN cloud repo: `valid/`
must satisfy `agent-gateway-v0.schema.json`, and every file in `invalid/` must
not. The build verifies the manifest's `schema_sha256` against the schema on
every configure (`components/device_protocol/CMakeLists.txt`), so a schema edit
that is not mirrored into `manifest.json` fails the build instead of drifting.

`RunContractFixtureSelfTest()` replays the same frames through the firmware's
own parsers at boot: the event-name → kind dispatch, the question and reasoning
payloads, and the history response. The negative cases are the fixture literals
mutated into malformed values, so a parser that starts accepting junk fails the
self-test. Mirroring the fixture text both here and in
`main/contract_fixtures.cpp` is what keeps that meaningful — changing one
without the other makes the hash check pass while testing nothing.
