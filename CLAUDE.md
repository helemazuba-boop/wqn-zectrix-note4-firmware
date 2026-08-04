# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESP-IDF firmware for the **WQN Note4** — an ESP32-S3, 400×300 e-paper study
terminal built on the `zectrix-s3-epaper-4.2` board. The device connects to the WQN
cloud over WiFi, caches due problems / todos / word decks locally, renders them to
e-paper, and uploads review results. Provider secrets (Supabase, ASR/LLM providers,
notebook permissions, AI function-call execution) stay server-side; the firmware only
talks to the **WQN ESP32 API** using a device-pairing token.

All firmware source lives under `firmware/wqn-zectrix-note4/`. The repo root also holds
`README.md` (Chinese, project overview + dev order), `LICENSE` (GPL-3.0), and
gitignored local dirs `analysis/` (official-firmware reverse engineering), `docs/`
(local spec/handoff notes), and `.claude/` / `.opencode/` / `.vscode/` (tool state).
The detailed English firmware README is `firmware/wqn-zectrix-note4/README.md`;
`firmware/wqn-zectrix-note4/BUILD_HERE.md` is the build/flash playbook. Inline code
comments are mixed Chinese/English; identifiers are English.

> Environment note: the tracked READMEs and `BUILD_HERE.md` were written for the
> maintainer's Windows workspace and reference `D:\projects\wqn-zectrix-note4-firmware`
> plus `D:\Program\Espressif\frameworks\esp-idf-v5.5.4`. This checkout lives at
> `/home/unknow/projects/firmware` (WSL). The `idf.py` commands below are
> environment-agnostic once the ESP-IDF env is activated; adjust the `cd` and IDF
> activation path to wherever ESP-IDF lives in the current environment.

## Build, flash, monitor

ESP-IDF **v5.5.x** is required. **Always build into the `build-ai-local-s3` directory**
— it is the only build dir kept on purpose (see `BUILD_HERE.md`).

```sh
cd firmware/wqn-zectrix-note4
idf.py --no-ccache -B build-ai-local-s3 set-target esp32s3
idf.py --no-ccache -B build-ai-local-s3 build
```

Override the WQN API base for a dev backend (the ESP32 can't reach your `localhost`):

```sh
idf.py -B build-ai-local-s3 -DWQN_API_BASE=https://your-host.example.com/api/esp32 build
```

Default API base is `https://wqn.helema.cn/api/esp32` (`main/config.h`).

Full flash cycle (build → erase → flash → serial listener), from the firmware dir on
Windows:

```bat
deploy.bat
```

- **Dev port is `COM7`.** `COM5` is the official-firmware checkpoint; the portable
  flasher refuses `COM5`. Do not flash `COM5`.
- `deploy.bat` ends by running `listen_usb.py` (writes serial output to `wqn.log`)
  because **`idf.py monitor` is broken over the chip's Native USB-Serial-JTAG
  interface**. Use `listen_usb.py` / `scripts\monitor.bat` for live logs.
- Granular helpers live in `firmware/wqn-zectrix-note4/scripts/` (`flash-fix.bat`,
  `erase_reflash.bat`, `monitor.bat`, `read_flash.bat`, …). **Do not drop new `.bat`
  files in the repo root** — only `deploy.bat` belongs there.

### Configuration

Feature toggles live in `idf.py menuconfig` under **WQN firmware** (defined in
`main/Kconfig.projbuild`). The load-bearing ones:

- `CONFIG_WQN_WIFI_STA_ENABLE` — WiFi station + all WQN API calls (default **n**).
- `CONFIG_WQN_PROVISION_ENABLE` — SoftAP `ZECTRIX_XXXX` captive portal at
  `192.168.4.1` when no NVS WiFi creds exist (default y, depends on WiFi STA).
- `CONFIG_WQN_EPD_UI_ENABLE` — e-paper UI + button-driven app (default **n**).
- `CONFIG_WQN_EPD_LOCAL_PARTIAL_ENABLE` — Note4 `0x83` local partial-window refresh.
- `CONFIG_WQN_DEEP_SLEEP_ENABLE` — deep sleep after UI idle (wake: GPIO 0/18/5).
- `CONFIG_WQN_AI_ENABLE` — AI modules (SSE streaming + Flash realtime voice); provider
  secrets still server-side.
- `CONFIG_WQN_AI_AUDIO_SELFTEST_ENABLE` — capture a short mic sample at boot, print
  RMS/peak, never upload.

Defaults are **layered**: the root `CMakeLists.txt` appends both `sdkconfig.defaults`
**and** `sdkconfig.ai-local.defaults` to `SDKCONFIG_DEFAULTS`. The `ai-local` profile
is what turns on WiFi STA + AI + EPD UI + power management — omit it and `idf.py`
silently drops the PM/deep-sleep config and the device "becomes a 100 mA heater."
`sdkconfig.ai-local.defaults` is the in-house full-feature build profile.

### Tests

There is no general host-side test runner. The build itself runs the M8 ownership
gate, and boot-time device self-tests are read over serial:

- `cmake/verify_architecture.cmake` runs on every build and enforces feature/HAL
  layering plus unique EPD, WiFi, storage, audio and deep-sleep entry points.

- `wqn::RunContractFixtureSelfTest()` (`contract_fixtures.cpp`) runs every boot and
  validates the WQN API JSON parsing fixtures. Failure is logged, not fatal.
- `CONFIG_WQN_AI_AUDIO_SELFTEST_ENABLE` captures a short mic sample at boot and prints
  RMS/peak stats; it never uploads.

To exercise a feature: enable its Kconfig flag, build, flash, read `wqn.log`. A
diagnostic should not require a reflash to confirm.

## Architecture

`app_main` (`main.cpp`) performs startup assembly only. It establishes safe pins and
shared buses, captures `WakeContext`, enforces storage schema generation 3, starts
services/features, then starts `PowerCoordinator`; it does not poll business state or
enter sleep itself.

The enforced dependency direction is features → service interfaces → services →
runtime primitives → Note4 HAL/ESP-IDF. See
`firmware/wqn-zectrix-note4/ARCHITECTURE.md` for the full ownership map. Every build
runs `cmake/verify_architecture.cmake` and rejects driver/HAL access from features,
second hardware owners, or a second deep-sleep call site.

**Extracted IDF components** — `platform_note4` owns boot safe pins;
`power_runtime` contains `SleepLease`, `WakeContext`, snapshots and sleep protocol;
`device_protocol` contains v3 contract/claim crypto; `display_service` owns the EPD
framebuffer, SPI, BUSY and GPIO6. `display_service` depends only on `power_runtime`;
the other three are independent. High-level `PowerCoordinator` remains in `main`
because it coordinates service interfaces and extracting it would create a cycle.

**Services** — `ConnectivityService` owns station/provision/reconnect policy;
`StorageService` serializes NVS/SPIFFS writes; `AudioService` owns I2S, ES8311 and the
amplifier; `SyncService` owns claim/bootstrap/sync lifecycle and publishes fixed-size
domain events. `wifi_manager`, the provisioning component, `storage.cpp` and
`word_pack.cpp` are low-level adapters used behind those services, not feature-owned
tasks.

**UI subsystem (`main/ui/`)** — `device_ui.cpp` starts a single-owner `UiRuntime`.
Button, timer, cloud and display results are events reduced into `AppState`; effects
produce bounded refresh/cloud commands. `ui_refresh.cpp` has two fixed frame slots and
submits versioned intents to `DisplayService`. Screen renderers live in `page_*.cpp`;
`ui_layout.h` remains the geometry source of truth. UI reads immutable power, storage,
connectivity and sync snapshots and must not include ESP-IDF driver headers.

**Transport and media** — control plane uses `/api/esp32/v3/*` and protocol header 3.
AI SSE and `wqn-flash-v2` retain their existing wire layouts but use the common
authentication/session/cancel lifecycle. Firmware never connects to Supabase directly.

### Key runtime concepts

- **Refresh terminal results.** Every accepted display revision ends in exactly one
  `Presented`, `Superseded` or `Failed`. The service keeps one active and one merged
  pending intent, strengthens waveform requirements, unions dirty regions and commits
  RTC panel state only after BUSY completes. One reset/re-init/full retry is allowed;
  cold/untrusted wake and every 20th partial force a full refresh.
- **Sleep transaction.** Work holds move-only `SleepLease` objects. After 60 seconds
  idle with no lease, `PowerCoordinator` closes admission and runs generation-tagged
  prepare/rollback across display, storage, audio, connectivity and wake controller.
  It is the only caller of `esp_deep_sleep_start()`. USB/charger presence intentionally
  holds a lease and blocks automatic light/deep sleep while monitoring.
- **Cloud async pattern.** Todo/word screens never block the UI: `Queue…Request()`
  posts to a FreeRTOS queue, a dedicated `TodoCloudTask`/`WordCloudTask` does the
  `wqn_api` call off-thread, and `Apply…Result()` folds the result back into `UiState`.
  Mirror this for any new network-backed screen.
- **Sync service.** `services::StartSyncService()` owns claim → bootstrap → sync and
  parks on `portMAX_DELAY` when appropriate. Wake it through
  `services::RequestSyncNow()`; consumers receive fixed-size `SyncEvent` values or an
  immutable `SyncSnapshot`.
- **Security boundary.** HTTP **401 is the only condition that clears the stored token**
  and returns the device to pairing. Any other server/provider error is surfaced to the
  user without destroying pairing state. Never embed provider keys in firmware.

## Hardware constraints (read before touching pins or power)

Bootstrap pin mapping lives in `components/platform_note4`; runtime EPD/audio/wake
pins live only in their owning services. The load-bearing rules:

- **GPIO 17 board-power latch** (`kBoardPowerLatch`) must stay HIGH through light/deep
  sleep (`gpio_hold_en` + `gpio_sleep_sel_dis`); if it releases during sleep the board
  reboots in a ~1.6 s loop.
- **One I2C bus only: `I2C_NUM_0`, SDA=GPIO 47, SCL=GPIO 48.** PCF8563 RTC (0x51),
  ST25DV NFC (0x55), ES8311 audio (0x18) all share it. Get the handle via
  `wqn::GetSharedI2cBusHandle()` — never call `i2c_new_master_bus` from a peripheral
  driver, or the second caller panics with `ESP_ERR_INVALID_STATE`.
- **Deep-sleep wake sources are active-low GPIO 0 (confirm), 18 (page-down), 5 (RTC
  INT), 2 (charge) and 1 (full)** via ext1. GPIO 39 (page-up) cannot wake from deep
  sleep on S3 and is deliberately absent.
- **GPIO 18 is the page-down / KEY_DET key with an external pull-up** — despite the
  source name `kPageDownAndPowerDetect`, it is *not* a USB-power-detect pin. A PC
  connection is detected from USB Serial/JTAG SOF frames; charger-only power is
  inferred from GPIO 2 `CHRG_L` or GPIO 1 `/STDBY` (both active-low). Do not use
  charger state alone to decide whether native USB may sleep.
- **GPIO 19 / 20 are USB D− / D+.** Never repurpose them as GPIO/I2C — doing so kills
  the USB link to the PC. Leave them to the USB controller.
- The **EPD rail (GPIO 6)** is controlled only by `DisplayService`; before cutting it,
  send the panel deep-sleep command. Battery ADC/GPIO values are exposed only as
  `PowerStatusSnapshot`, never as driver handles to UI.

## Conventions

- **Serial output: `ESP_LOGx` only.** Do not call `uart_write_bytes` from `main/` — it
  collides with the console UART driver and breaks serial logging.
- **`// [power-fix]` comments** mark recently-fixed power/PM hotspots with the measured
  "why." Preserve them and match the style for related fixes — they encode
  battery-drain context.
- **`device_ui.cpp` is large and being split into `ui/`.** Prefer a new `ui/page_*` /
  sub-module over growing `device_ui.cpp` inline. `ui_layout.h` tokens replace
  scattered magic numbers.
- **One build dir** (`build-ai-local-s3`). If you genuinely need another, name it
  `build-<purpose>-<date>` and document it in `BUILD_HERE.md`. Scripts go in `scripts/`,
  not the repo root.
- **No secrets in firmware**: no Supabase service-role keys, no provider (ASR/LLM/
  DashScope) keys, no committed WiFi passwords or tokens. `sdkconfig` (which may hold
  dev WiFi creds) is gitignored.
- `sdkconfig`, `build-*/`, `dist/`, `analysis/`, `docs/`, `logs/`, `*.log`, and `*.bin`
  are all gitignored local artifacts.

## Project context outside this checkout

This firmware is the **device side (端)** of a three-repo product. The sibling repos
live in the maintainer's broader workspace (not in this checkout): the **WQN cloud**
(Next.js + Supabase, original `wqn.magicworks.app` product; hosts `/api/esp32/*` route
handlers, ASR/LLM providers, notebook permissions, AI function-call execution) and an
**official-firmware reference** (`ESP32DOC`: official ZecTrix dumps, schematic PDF,
Ghidra project, RE reports). When a request/response shape changes, update the
authoritative JSON Schema and golden fixtures in WQN, its pinned firmware copy/hash,
C++ parsers and TypeScript validation together. The control contract is
`/api/esp32/v3/*`, `X-WQN-Protocol: 3`, with `request_id`, `boot_id`, capabilities and
`{ok,...}` success/error envelopes. Device claim uses ephemeral P-256
ECDH/HKDF/AES-GCM; MAC only locates a candidate and never recovers a credential.
Runtime auth remains `Authorization: Bearer <token>` and cloud storage retains only
its hash. **401 = clear token; timeout/429/5xx = keep identity.**
