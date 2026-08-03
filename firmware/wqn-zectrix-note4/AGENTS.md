# AGENTS.md — WQN ZecTrix Note4 Firmware

Operating rules for any AI agent (or human) changing this firmware. It is
**self-contained**: build/flash, architecture, hardware limits, the concurrency
invariants that took months of review to establish, and the review workflow are
all here. When it disagrees with older docs, **this file + the code win**.

Companion docs (read for depth, not for rules): `README.md` /
`README.zh-CN.md` (overview), `ARCHITECTURE.md` (ownership map),
`BUILD_HERE.md` (flash playbook), `CLAUDE.md` at the repo root (overlapping
guidance written for the maintainer's Windows workspace).

---

## 0. TL;DR — the rules you cannot break

1. **The UI task does NO synchronous storage writes and NO direct EPD hardware
   calls.** Local writes go to the persist worker; panel operations go to the
   EPD owner task. (§4.1)
2. **`commit_state == kPersisting` is the unified gate** guarding the whole
   Prepare→reserve→apply window. Any path that resets a session or reads
   storage synchronously must check it. (§4.2)
3. **Cross-task scalars are `std::atomic`; publish payload before the flag**
   (release/acquire pairing). (§4.7)
4. **Deep sleep must not eat a just-produced input.** Activity generation is
   published at button *production* before the event enters the ring, and
   re-validated before quiesce and again inside the sleep commit. (§4.5)
5. **Single hardware owners, single `esp_deep_sleep_start()` call site** — the
   build's M8 gate rejects violations. Only HTTP **401** clears the token. (§4.9)
6. **Every change must build with 0 warnings, pass the M8 gate, and leave
   `git -c core.whitespace=cr-at-eol diff --check` clean** before you present or
   commit it. (§6)
7. **High-risk changes (sleep, power, EPD hardware, storage protocol) require
   on-device HIL verification before commit**, and commits land only after
   review approval. (§7)
8. **Never push or open a PR unless explicitly asked.** (§7)

---

## 1. What this is

ESP-IDF firmware for the **WQN Note4** — an ESP32-S3, 400×300 e-paper
study terminal on the `zectrix-s3-epaper-4.2` board. It connects to the WQN
cloud over WiFi, caches due problems / todos / word decks locally, renders them
to e-paper, and uploads review results. Provider secrets (Supabase, ASR/LLM,
notebook permissions, AI function-call execution) stay server-side; the firmware
only talks to the **WQN ESP32 API** with a device-pairing token.

All source lives under `firmware/wqn-zectrix-note4/`. Inline comments are mixed
Chinese/English; identifiers are English. This firmware is the **device side**
of a three-repo product (device / WQN cloud / official-firmware reference).

---

## 2. Build, flash, monitor

ESP-IDF **v5.5.x** required.

**This (WSL) checkout** builds into `build-wsl`:

```sh
cd firmware/wqn-zectrix-note4
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl build
```

> The tracked READMEs / `BUILD_HERE.md` / `CLAUDE.md` were written for the
> maintainer's Windows workspace and use `build-ai-local-s3` + a `D:\...`
> ESP-IDF path. Both are valid; **match whichever build dir already exists in
> the current checkout** and do not create a second one casually. If you truly
> need another, name it `build-<purpose>-<date>` and note it in `BUILD_HERE.md`.

Override the WQN API base for a dev backend (the ESP32 can't reach your
`localhost`):

```sh
idf.py -B build-wsl -DWQN_API_BASE=https://your-host.example.com/api/esp32 build
```

Default API base is `https://wqn.helema.cn/api/esp32` (`main/config.h`).

Flash/monitor: `idf.py monitor` is **broken** over the chip's Native
USB-Serial-JTAG. Use the serial listeners in `scripts/` (they write to
`wqn.log`). On the Windows workspace `deploy.bat` runs the full cycle; **dev
port is `COM7`, never flash `COM5`** (official-firmware checkpoint). Do not add
new `.bat` files to the repo root — only `deploy.bat` belongs there; helpers go
in `scripts/`.

### Configuration (Kconfig, under "WQN firmware")

Defaults are **layered**: the root `CMakeLists.txt` appends both
`sdkconfig.defaults` **and** `sdkconfig.ai-local.defaults`. The `ai-local`
profile turns on WiFi STA + AI + EPD UI + power management — omit it and the
PM/deep-sleep config silently drops and the device "becomes a 100 mA heater."

Load-bearing flags: `CONFIG_WQN_WIFI_STA_ENABLE` (default n),
`CONFIG_WQN_PROVISION_ENABLE` (SoftAP `ZECTRIX_XXXX` at `192.168.4.1`),
`CONFIG_WQN_EPD_UI_ENABLE` (default n), `CONFIG_WQN_EPD_LOCAL_PARTIAL_ENABLE`
(`0x83` local partial), `CONFIG_WQN_DEEP_SLEEP_ENABLE`, `CONFIG_WQN_AI_ENABLE`,
`CONFIG_WQN_AI_AUDIO_SELFTEST_ENABLE`, `CONFIG_WQN_EPD_IDLE_POWER_OFF_MS`.

### Tests

No general host-side test runner (CTest registers 0 tests). Verification is:

- **Build-time M8 gate** — `cmake/verify_architecture.cmake` runs on every build:
  enforces feature/HAL layering and unique EPD / WiFi / storage / audio /
  deep-sleep entry points. A violation fails the build.
- **Boot self-tests over serial** — `wqn::RunContractFixtureSelfTest()` validates
  WQN API JSON fixtures every boot (logged, not fatal); the per-domain app
  self-tests (word/note/problem) run in-process; the audio self-test prints
  RMS/peak and never uploads.
- **Concurrency windows (sleep race, pool reuse, EPD ownership, marker recovery)
  have no host test — they are HIL / fault-injection only.** Treat them with
  extra care (§7).

---

## 3. Architecture (ownership)

`app_main` (`main.cpp`) does startup assembly only: safe pins, shared buses,
`WakeContext` capture, storage schema generation 3 enforcement, start
services/features, then start `PowerCoordinator`. It never polls business state
or sleeps itself. **A boot-time failure in a storage-integrity/recovery step
must abort into storage recovery mode, not continue into business services.**

Enforced dependency direction (M8 gate): features → service interfaces →
services → runtime primitives → Note4 HAL/ESP-IDF. See `ARCHITECTURE.md`.

**Extracted IDF components** — `platform_note4` owns boot safe pins;
`power_runtime` holds `SleepLease` / `WakeContext` / snapshots / sleep protocol;
`device_protocol` holds v3 contract/claim crypto; `display_service` owns the EPD
framebuffer, SPI, BUSY and GPIO6. `display_service` depends only on
`power_runtime`; the other three are independent. `PowerCoordinator` stays in
`main` (extracting it would create a cycle). **Components must not depend back on
`main`** — e.g. `display_service` learns the EPD owner task via a registration
call, never by referencing a `main` symbol.

**Services** — `ConnectivityService` (station/provision/reconnect),
`StorageService` (serializes NVS/SPIFFS writes), `AudioService` (I2S/ES8311/amp),
`SyncService` (claim/bootstrap/sync lifecycle, fixed-size domain events).

**UI subsystem (`main/ui/`)** — `device_ui.cpp` runs a single-owner `UiRuntime`.
Button / timer / cloud / display-result / persist-result events reduce into
`AppState`; effects produce bounded refresh/cloud/persist commands. Screen
renderers live in `page_*.cpp`; `ui_layout.h` is the geometry source of truth.
UI reads immutable power/storage/connectivity/sync snapshots and must not include
ESP-IDF driver headers.

**Task inventory** (know who runs what before you add work to a stack):
`app_main`/IDF main, `PowerCoordinator`, `UiRuntime` (device UI), EPD refresh
task (`wqn_epd_refresh`, the sole panel owner), two cloud lanes
(`wqn_cloud_int` interactive / `wqn_cloud_blk` bulk), the persist worker,
`SyncService`, `ConnectivityService`, `AudioService`, the ISR-fed button task.

---

## 4. Concurrency & ownership invariants (the crown jewels)

These are the rules that took the async-I/O + deep-sleep + EPD-owner work to
establish. Breaking one reintroduces a UI freeze, a lost input, a torn write, or
a hardware hang that **no host test will catch**. Each rule states the *why*.

### 4.1 The UI task never blocks on storage or touches the panel

The UI task must not call a synchronous NVS/SPIFFS write or an EPD hardware op
directly. Storage I/O on the Note4 can stall for 1–3 s (session snapshots,
pack writes); doing it on the UI task froze the screen and dropped buttons.

- **Local durable writes** → submit to the **persist worker** (`ui/persist_worker.*`).
- **Panel operations** (draw/refresh/cleanup/power-off) → the **EPD owner task**
  (`ui/ui_refresh.cpp` + `display_service`).
- The persist worker's storage entries run **foreground** (bounded queue wait)
  rather than background; note that "foreground" bounds only the *queue* wait —
  once a transaction starts it may still wait without a fixed deadline. Do not
  claim otherwise in comments.

### 4.2 `commit_state == kPersisting` is the unified gate

Each observation/verdict domain (word/note/problem) carries a `commit_state`.
`kPersisting` spans the **whole** Prepare → reserve → worker-apply window, which
is **wider** than "persist worker busy" (it also covers the Prepare→reserve gap
where the effect is armed but not yet enqueued).

Any path that (a) resets/clears a session, (b) reads storage synchronously on
the UI task, or (c) navigates away from the answering context must gate on
`commit_state == kPersisting`, not on persist-busy. This includes: periodic
status reload, sync-success outbox read (peek, don't consume), settings-page
Confirm, default-deck switch, and top-bar navigation (`kTopPrevious/kTopNext`)
away from a scoped word page or an active problem verdict.

### 4.3 Persist worker: two-phase reserve + validated ACK

- **Two-phase**: `TryReservePersist(kind)` (reserves busy + slot + SleepLease)
  → take the effect from UI state → `EnqueueReserved…(ticket, payload)` **or**
  `CancelPersistReservation(ticket)`. Reserve **before** taking the effect, so a
  failed reservation never costs the armed effect.
- **ACK mailbox** is per-kind and validates `{generation, operation_id, slot
  ownership}` before it CASes `ResultPending → Free` and clears busy. A late or
  mismatched result is dropped, never applied.
- **SleepLease lifetime ≠ busy lifetime**: the lease is released when the
  storage write ends; busy clears only after the UI acks. They are two separate
  lifetimes — do not fuse them.
- After the storage call, the worker **clears the command payload** before
  publishing the result (pool slots otherwise pin hundreds of KB of
  session/candidate data).

### 4.4 Result application is operation_id-bound

Every async result (persist or cloud session/page) carries the `operation_id`
(or scope epoch) sampled when the request was armed. On apply, compare it to the
value still bound in the target state; if it no longer matches (session reset,
newer submit, scope change), **drop the result** — do not fold it into current
state. This is what stops a stale in-flight result from clobbering fresh state.

### 4.5 Deep sleep must never lose a just-produced input

The power coordinator sleeps after idle; a button pressed in the sleep window
must cancel the sleep, not vanish with the RAM image. The linearization point is
**button production**, not UI consumption:

- `PushButtonEvent` publishes the activity generation (via `NoteUserActivityAtMs`)
  **before** the critical (non-repeat) event enters the ring, so
  **"event queued ⇒ generation advanced" always holds** (publish payload/flag in
  that order; §4.7).
- The sleep path samples the generation **before** the idle/token checks,
  re-validates **before** `TryBeginSleepQuiesce`, again **after** quiesce, and a
  **final** time inside `CommitDeepSleep` after the UART flush + settle delay,
  under `g_activity_gate` (the same critical section `NoteUserActivity`
  publishes through) so no bump can land between the check and
  `esp_deep_sleep_start()`.
- An accepted display intent counts as EPD activity (bumps the EPD activity
  generation) — both the primary and secondary accept paths do it; rejected
  intents do not.
- Repeat (held-key) events do not publish activity: the key is physically held,
  so the wake source stays asserted anyway.

### 4.6 EPD is a single-owner resource

The EPD refresh task is the **sole** owner of draw/refresh/cleanup/power-off.

- Wrap the whole clear→draw→refresh of a frame in the RAII
  `EpdFrameTransaction` (recursive mutex; inner `RefreshEpdFull` re-enters
  harmlessly). Check `locked()` and bail on failure — the mutex handle can be
  null on allocation failure; never draw unprotected.
- **Idle maintenance** (heavy-partial cleanup full refresh + rail power-off) and
  **sleep preparation** run only at the EPD task's idle command point (no
  pending frame, no transaction held). Other tasks *request*, they do not
  execute.
- Requests are **edge-triggered** (gate on "actually due" + false→true CAS) so an
  idle poll can't wake the task every cycle.
- Sleep-prep uses a **Pending → Claimed → Idle** state machine + a
  generation-tagged completion mailbox: a power-side timeout may cancel only
  `Pending → Idle`; once the owner Claims it, the power-off runs to completion
  and a late timeout cannot masquerade as cancelling an in-progress hardware op.
  The requester recomputes remaining budget from the **absolute** deadline
  before each wait (no double budget). An owner-task caller runs locally
  (fast-path) to avoid self-deadlock. Sleep-prep is serviced **before** idle
  maintenance and skips maintenance for the round when it claims a command, so a
  deadline-bound power-off never queues behind a 1–3 s cleanup.

### 4.7 Cross-task shared state is atomic, published in order

Any scalar written by one task and read by another (activity timestamps, volume
cache, generations, request flags) is `std::atomic` — a plain `int64_t`/`int`
can tear on this 32-bit core. When a value + a "ready" flag are published
together, **write the payload first (relaxed/release), then the flag (release);
read the flag (acquire) then the payload**. The consumer that observes the flag
set must be guaranteed to see the payload. Getting this backwards silently
cancels the next command or applies a stale one.

For an RTC-persisted scalar that must stay a plain variable, use
`std::atomic_ref` at the access sites rather than changing its storage.

### 4.8 Recoverable protocol for cross-store multi-step changes

A change that must touch **both** NVS and SPIFFS (or several keys) with no
cross-store atomicity uses a **marker protocol**, not a bare sequence:
write marker → do the steps → clear marker; boot recovery replays the
idempotent tail from a leftover marker and a recovery failure aborts startup.
Stamp affected records with a **generation** and reject stale-generation
loads/saves. The default-deck switch is the reference implementation
(`ChangeDefaultWordDeckForeground` / `RecoverDefaultDeckScopeChange` /
`deck_scope_generation`, plus the three-line defense: UI defer while the switch
persists, save-side reject of a stale-scope session, apply-side drop of a
straddling cloud result). Parse markers strictly (full consume, bounded
generation, valid id) — never replay a corrupt marker as a real change.

### 4.9 Hard singletons enforced by the build

- **One `esp_deep_sleep_start()` call site** in the whole firmware. The M8 gate
  counts textual occurrences — even a comment containing `esp_deep_sleep_start(`
  trips it, so write "the deep-sleep entry" in prose.
- **One owner per hardware resource** (EPD, WiFi, storage, audio). No second
  `i2c_new_master_bus`, no driver handle handed to UI.
- **401 is the only condition that clears the stored token** and returns to
  pairing. Any other error (timeout/429/5xx) surfaces to the user but keeps
  pairing state. Never embed provider keys.

---

## 5. Async patterns to mirror

- **Cloud** (`ui/cloud_runner.cpp`): two lanes — interactive (`wqn_cloud_int`:
  session start, candidate page, image fetch, todo refresh) and bulk
  (`wqn_cloud_blk`: multi-MB pack sync). A slow bulk sync must not block
  interactive requests. Results use the **ack-mailbox** handshake:
  `PublishCloudResult` → UI `TakeCloudResultToApply` → apply → `AckCloudResult`;
  busy clears only after the UI acks (no silent terminal-result loss). Mirror
  this for any new network-backed screen; do not reintroduce a shared result
  queue.
- **Sync** (`services::StartSyncService`): owns claim → bootstrap → sync; parks
  on `portMAX_DELAY`; wake via `services::RequestSyncNow()`; consumers get
  fixed-size `SyncEvent` values or an immutable `SyncSnapshot`.
- **Refresh terminal results**: every accepted display revision ends in exactly
  one `Presented` / `Superseded` / `Failed`. One reset/re-init/full retry is
  allowed; cold/untrusted wake and every Nth partial force a full refresh.
- **Async settings save**: Confirm arms `pending_* + *_pending_valid` (0 is a
  valid value, so a flag — not a zero sentinel — marks "armed"), submits to the
  worker, and installs the value only on the durable ACK; a submit reject or
  write failure keeps the armed value for a re-Confirm. A runtime cache (e.g.
  volume) is updated on the UI thread at submit; the durable read path
  (`LoadVolumePercent`) must be a **pure read** that does not write the cache
  back, or it silently rolls the value back.

---

## 6. The pre-commit acceptance gate (run every time)

Before presenting a change as done or committing it:

```sh
cd firmware/wqn-zectrix-note4
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-wsl build        # EXIT=0, 0 warnings, "M8 ownership gate passed"
git -c core.whitespace=cr-at-eol diff --check   # clean (CRLF-aware; see §7)
```

- **0 warnings is the bar**, not just 0 errors (`-Werror` is not everywhere;
  treat any `warning:` as a failure to fix).
- The M8 line must read "passed".
- Report outcomes faithfully: if a step failed or was skipped, say so with the
  output. Do not claim HIL was done when it wasn't.

---

## 7. Review & commit workflow

- **Separate commits per logical unit.** Do not merge unrelated changes.
  `note` and `problem` migrations were required to be separate commits; keep
  that discipline. Infra, then per-domain, then cleanup.
- **EOL discipline**: some baseline files are CRLF (`note_app.cpp/.h`,
  `note_cloud.cpp`). Keep their CRLF in feature commits; verify with
  `git -c core.whitespace=cr-at-eol diff --check`. If you must normalize line
  endings, do it as a **separate `chore` commit**, never mixed into a feature
  diff.
- **High-risk changes require HIL before commit.** Anything touching deep
  sleep, power, EPD hardware timing, or a storage protocol has no host test —
  build-green is necessary but not sufficient. State the HIL matrix and get it
  run (e.g. for sleep-prep: pending timeout, claimed near deadline,
  stale-completion interleave, maintenance+sleep-prep coincident, EPD-raised
  emergency, EPD-busy / normal idle sleep / wake restore).
- **Commit only after review approval**; when a reviewer is in the loop, present
  the change and wait. Do not self-approve high-risk work.
- **Never push or open a PR unless explicitly asked.** Never force-push, never
  push to `main`, never skip hooks, never touch git config.
- **Commit messages**: imperative subject scoped by area (`refactor(display):`,
  `fix(power):`, `feat(settings):`, `chore(cloud):`); body explains the *why*
  and the invariant preserved, not just the diff.

---

## 8. Conventions & footguns

- **Serial output: `ESP_LOGx` only.** Never `uart_write_bytes` from `main/` — it
  collides with the console UART driver and breaks logging.
- **`// [power-fix]`, `// [sleep-race]`, `// [epd-owner]`, `// [persist-worker]`,
  `// [deck-scope]` comments** encode the measured "why" behind a hotspot.
  Preserve them; match the tag when extending the same fix.
- **`device_ui.cpp` is large and being split into `ui/`.** Prefer a new
  `ui/page_*` / sub-module over growing it inline. `ui_layout.h` tokens replace
  magic numbers.
- **The `SearchReplace`/edit tool occasionally reports "save failed" when the
  write actually landed (or partially).** Before retrying, `grep` the file to
  confirm real state — a blind retry can duplicate a block.
- **No secrets in firmware**: no Supabase service-role keys, no ASR/LLM/DashScope
  keys, no committed WiFi passwords or tokens. `sdkconfig` (may hold dev WiFi
  creds), `build-*/`, `dist/`, `analysis/`, `docs/`, `logs/`, `*.log`, `*.bin`
  are gitignored local artifacts.
- **UI focus decoration** is only invert-black/white or the rounded double-line;
  device pages keep no bottom status/hint row (design language constraints).

---

## 9. Hardware constraints (read before touching pins or power)

Bootstrap pins live in `components/platform_note4`; runtime EPD/audio/wake pins
live only in their owning services. Load-bearing:

- **GPIO 17 board-power latch** must stay HIGH through light/deep sleep
  (`gpio_hold_en` + `gpio_sleep_sel_dis`); if it releases the board reboots in a
  ~1.6 s loop.
- **One I2C bus: `I2C_NUM_0`, SDA=47, SCL=48.** PCF8563 RTC (0x51), ST25DV NFC
  (0x55), ES8311 (0x18) share it. Get the handle via
  `wqn::GetSharedI2cBusHandle()` — a second `i2c_new_master_bus` panics with
  `ESP_ERR_INVALID_STATE`.
- **Deep-sleep wake sources (ext1, active-low): GPIO 0, 18 (page-down), 5 (RTC
  INT), 2 (charge), 1 (full).** GPIO 39 (page-up) cannot wake from deep sleep on
  S3 and is deliberately absent — a released short-press whose GPIO is no longer
  low cannot re-arm ext1, which is *why* §4.5 matters.
- **GPIO 18** is page-down / KEY_DET with an external pull-up — despite the
  source name it is *not* USB-power-detect. PC connection is detected from USB
  Serial/JTAG SOF frames; charger-only power from GPIO 2 `CHRG_L` / GPIO 1
  `/STDBY` (active-low).
- **GPIO 19/20 are USB D−/D+.** Never repurpose them.
- **EPD rail (GPIO 6)** is controlled only by `DisplayService`; before cutting
  it, send the panel deep-sleep command. Battery ADC is exposed only as
  `PowerStatusSnapshot`, never as a driver handle to UI.

---

## 10. Product context outside this checkout

Device side of a three-repo product. Siblings (not in this checkout): the **WQN
cloud** (Next.js + Supabase; hosts `/api/esp32/*`, ASR/LLM providers, notebook
permissions, AI function-call execution) and an **official-firmware reference**
(RE dumps/schematic/Ghidra). When a request/response shape changes, update the
authoritative JSON Schema + golden fixtures in WQN, its pinned firmware
copy/hash, the C++ parsers and the TypeScript validation **together**. Control
contract: `/api/esp32/v3/*`, `X-WQN-Protocol: 3`, `request_id` / `boot_id` /
capabilities / `{ok,...}` envelopes. Claim uses ephemeral P-256
ECDH/HKDF/AES-GCM; MAC only locates a candidate and never recovers a credential.
Runtime auth is `Authorization: Bearer <token>`; cloud keeps only its hash.
**401 = clear token; timeout/429/5xx = keep identity.**
