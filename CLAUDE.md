# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESP-IDF firmware for the **WQN ZecTrix Note4** — an ESP32-S3, 400×300 e-paper study
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

There is **no host-side test framework**. "Tests" are boot-time device self-tests,
gated by Kconfig and read over serial:

- `wqn::RunContractFixtureSelfTest()` (`contract_fixtures.cpp`) runs every boot and
  validates the WQN API JSON parsing fixtures. Failure is logged, not fatal.
- `CONFIG_WQN_AUDIO_SELFTEST_ENABLE` captures a short mic sample at boot and prints
  RMS/peak stats; it never uploads.

To exercise a feature: enable its Kconfig flag, build, flash, read `wqn.log`. A
diagnostic should not require a reflash to confirm.

## Architecture

`app_main` (`main.cpp`) runs a fixed bring-up sequence, then loops calling
`EnterDeepSleepIfEnabled()` once a second. All other work is FreeRTOS tasks. Layers,
hardware up:

**Board / power** — `board_zectrix_note4.cpp::InitZectrixNote4SafePins()` is the single
GPIO entry point: powers off EPD/audio/amp/NFC, latches board power (GPIO17 HIGH),
configures inputs, then calls `power_manager.cpp::InitPowerHardware()`, which owns the
**one** I2C bus and the ADC. `power_manager` also handles light/deep-sleep entry,
wake-cause logging, and battery protection. `diagnostics.cpp` prints boot
chip/flash/MAC/reset info.

**Connectivity** — `wifi_manager.cpp` (station connect + backoff) and
`provision_manager.cpp` (SoftAP captive portal when no NVS WiFi creds). `wqn_api.cpp`
(+ `wqn_api_stream.cpp`) is the large transport + JSON layer: pairing (`/poll`),
Bearer-token auth, problem sync / `/problems` / `/review-complete`, Todo timeline,
word sync/review/search/packs, and AI audio chat (v2 SSE streaming + legacy v1
one-shot). Every fetch has a paired `Parse…Response`. `flash_session.cpp` implements
the `wqn-flash-v2` realtime voice WebSocket session. A vendored
`components/esp_websocket_client/` provides the WS transport.

**Storage** — `storage.cpp` wraps NVS namespace `wqn`: access token, problem cache,
pending-review queue, AI session cache, WiFi creds, settings (auto-sync interval,
volume). Tokens are masked in logs. Word packs (`.wpack`) live in the SPIFFS `storage`
partition (`partitions/16m.csv`: 16 MiB, two OTA slots + 8 MiB SPIFFS at 0x800000).

**EPD driver** — `epd_display.cpp` drives the Note4 controller over SPI with a 1bpp
framebuffer (`GetEpdFramebuffer()`): **1 = white, 0 = black, MSB-first, row-major.**
Full refresh + Note4 local partial-window path (`0x83`), plus EPD-rail power-down on
GPIO6 after idle (`PowerOffEpdAfterIdleIfNeeded`).

**UI subsystem (`main/ui/`)** — `device_ui.cpp` is the public entry
(`StartDeviceUiIfEnabled()`); implementation is split across `ui/`. **`ui/ui_internal.h`
is the shared header that ties the translation units together — read it first.**
`ui_model.{h,cpp}` defines the central `UiState` (current `UiScreen`, plus
time/todo/word/AI/settings sub-state). The loop is `ui_input.cpp` (button →
`ApplyButtonEvent`) → `ui_state.cpp` (mutate state) → `ui_render.cpp` + `page_*.cpp`
(draw to framebuffer) → `ui_refresh.cpp` (coalesce + dispatch). Screen renderers:
`page_home`, `page_time`, `page_todo`, `page_word`, `page_ai`, `page_settings`.
`ui_layout.h` is the single source of truth for screen geometry — prefer its tokens
over magic numbers.

**Apps** — `time_app.{cpp}` (clock/countdown/pomodoro), `word_app.cpp` +
`word_pack.cpp` (vocabulary SRS with local `.wpack` packs), `ai_session.cpp` +
`audio_capture.cpp` (long-press-to-record → upload 16 kHz mono PCM → render
transcript/reply), `audio_player.cpp` / `audio_volume.cpp` (ES8311 playback + persisted
volume), `pcf8563.cpp` (RTC, I2C 0x51).

### Key runtime concepts

- **Refresh scheduling.** UI code requests EPD refreshes through a `RefreshSchedule`
  priority enum (`kClock`, `kTimer`, `kSelection`, `kConfig`, `kAi`, `kCommit`,
  `kImmediate`). `ui_refresh.cpp` coalesces overlapping requests, ranks them, and waits
  the per-schedule cooldown before sending the next `0x83` window. When adding UI,
  request the weakest schedule that looks acceptable — the framework upgrades to a full
  refresh only when needed.
- **Cloud async pattern.** Todo/word screens never block the UI: `Queue…Request()`
  posts to a FreeRTOS queue, a dedicated `TodoCloudTask`/`WordCloudTask` does the
  `wqn_api` call off-thread, and `Apply…Result()` folds the result back into `UiState`.
  Mirror this for any new network-backed screen.
- **Online sync task** (`WqnOnlineTask`, `main.cpp`) runs pairing → upload-pending →
  sync-due → refresh-index on a configurable interval, and parks on `portMAX_DELAY`
  while unpaired to save power. Wake it with `RequestOnlineSyncNow()` (`xTaskNotifyGive`)
  whenever a token is saved or provisioning completes.
- **Security boundary.** HTTP **401 is the only condition that clears the stored token**
  and returns the device to pairing. Any other server/provider error is surfaced to the
  user without destroying pairing state. Never embed provider keys in firmware.

## Hardware constraints (read before touching pins or power)

Pin map is defined in `board_zectrix_note4.cpp` (constants `kEpdPower`, `kBoardPowerLatch`,
etc.) and `power_manager.cpp` (`kConfirmWake`, `kBatAdc`, …). The load-bearing rules:

- **GPIO 17 board-power latch** (`kBoardPowerLatch`) must stay HIGH through light/deep
  sleep (`gpio_hold_en` + `gpio_sleep_sel_dis`); if it releases during sleep the board
  reboots in a ~1.6 s loop.
- **One I2C bus only: `I2C_NUM_0`, SDA=GPIO 47, SCL=GPIO 48.** PCF8563 RTC (0x51),
  ST25DV NFC (0x55), ES8311 audio (0x18) all share it. Get the handle via
  `wqn::GetSharedI2cBusHandle()` — never call `i2c_new_master_bus` from a peripheral
  driver, or the second caller panics with `ESP_ERR_INVALID_STATE`.
- **Deep-sleep wake sources are RTC-capable GPIO 0 (confirm), 18 (page-down), 5 (RTC
  INT)** via ext1. GPIO 39 (page-up) **cannot** wake from deep sleep on S3, so it is
  deliberately not in the wake mask.
- **GPIO 18 is the page-down / KEY_DET key with an external pull-up** — despite the
  source name `kPageDownAndPowerDetect`, it is *not* a USB-power-detect pin. USB power
  is inferred from `IsCharging() || IsFullyCharged()` (GPIO 2 = `CHRG_L` low-active;
  GPIO 1 = `STDBY_H` high-active).
- **GPIO 19 / 20 are USB D− / D+.** Never repurpose them as GPIO/I2C — doing so kills
  the USB link to the PC. Leave them to the USB controller.
- The **EPD rail (GPIO 6)** is cut after idle; before power-off, send the panel
  deep-sleep command to stop leakage. Battery is read via the shared ADC (GPIO 4 /
  ADC1_CH3). `InitPowerHardware()` **must** run, or voltage reads 0 and low-battery
  shutdown misfires.

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
Ghidra project, RE reports). When a request/response shape changes, update it in both
the firmware (`wqn_api.cpp` + fixtures in `contract_fixtures.cpp`) and the cloud route
handler. The device↔cloud contract: `GET /poll` pairing returns `status` ∈
{`paired`,`already_paired`,`no_pending`,`expired`,`pending`}; runtime auth is
`Authorization: Bearer <token>` (64-char hex, MAC-bound, in NVS); envelope is
`{success,data,timestamp}` / `{success:false,error:{code,message},status,timestamp}`;
**401 = clear token, 500/other = keep pairing state**.
