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
remaining six carry data:

| Event | Data fields | Device effect |
| --- | --- | --- |
| `agent.status` | `status`, `message` | `idle` ends the stream and completes the run; `retry` is reported as a retry; anything else is "executing" |
| `agent.text.delta` | `delta` | Appends to the live assistant block |
| `agent.text` | `text` | Replaces the live assistant block |
| `agent.tool` | `tool`, `status`, `preview` | One history block per tool, coalesced by name |
| `agent.permission` | `permission_id`, `type`, `title`, `preview` | Enters `kAwaitingPermission`; the ask is answered by a separate POST |
| `agent.error` | `message` | Fails the run and surfaces the message |

**Termination.** The device reads until it sees `agent.status` with
`status: "idle"`. A stream that ends for any other reason — socket close,
timeout, upstream drop — is `stream_incomplete`, not a success: a run the user
believes finished but did not is worse than a visible failure. The stream has
a 5-minute socket timeout and the gateway is expected to keep it alive well
inside that.

**Permission replies are out-of-band.** While a stream is open the device
POSTs the reply on a short-lived second connection between stream reads, then
drains the queue again. It never opens a second task or a second long-lived TLS
session. If that POST fails, the pending ask is restored so it can be retried
rather than leaving the run blocked behind a silent failure — the device does
not assume the ask was answered.

## What the device does *not* mirror into history

A pending permission ask is rendered as a live overlay (dashed bubble + option
bar) and is deliberately not written to the local history. Its outcome already
appears as the tool blocks and text that follow, so mirroring it would show the
same decision twice.

## Bounds

These are device-side limits, not protocol requirements. The gateway should
stay well inside them.

| Bound | Value |
| --- | --- |
| Sessions listed | 12 |
| Assistant text per run | 12 KiB |
| Prompt per run | 4 KiB |
| JSON body | 16 KiB |
| SSE frame buffer | 768 B per read |
| Stream socket timeout | 5 min |

## Local history channels

Each tier owns its own `AiHistory` channel so a late background stream cannot
write into whichever tier happens to be visible. The Agent tier uses
`kAgent`; `kStdPro` and `kFlash` are unchanged. The channel is selected
explicitly at every call site rather than inferred from the current tier.

## Validation status

The JSON and SSE fixtures under `fixtures/` are the golden artifacts shared
with the WQN cloud repo. The firmware's own parsing is **not** yet covered by
`RunContractFixtureSelfTest()` — the agent dispatch and envelope parsing live
in `main/opencode_client.cpp`'s anonymous namespace, outside the
`components/device_protocol` layer the other contracts use. Extracting them
into an `agent_gateway_v0` parser next to `word_study_v1` is the follow-up that
would make this contract machine-checked on both sides.
