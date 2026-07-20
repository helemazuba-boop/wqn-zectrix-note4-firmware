# WQN ZecTrix Note4 Firmware

中文说明见 [README.zh-CN.md](README.zh-CN.md).

This directory contains the ESP-IDF firmware for the WQN ZecTrix Note4
companion device, an ESP32-S3 e-paper study terminal built around the
`zectrix-s3-epaper-4.2` board profile.

The firmware connects the physical device to the WQN cloud service while
keeping provider secrets and user data off the microcontroller. The device
handles local input, e-paper rendering, audio capture, lightweight caching, and
calls only the WQN ESP32 API. Cloud-only responsibilities such as ASR providers,
LLM providers, Supabase access, notebook permissions, and AI tool execution stay
on the server.

## Hardware Target

- MCU: ESP32-S3.
- Flash layout: 16 MiB, aligned with the ZecTrix Note4 baseline partitioning.
- Display: 400 x 300 e-paper panel using the Note4 controller command set.
- Input: physical buttons for page navigation, selection, timer control, and AI
  push-to-talk.
- Audio: ES8311/I2S microphone path for recording; provider processing is done
  by the WQN server.
- Power domains: explicit GPIO control for board hold, e-paper rail, audio
  power, amplifier, NFC, and low-power idle behavior.

## Firmware Roles

- Board bring-up and diagnostics: safe GPIO initialization, chip/flash/PSRAM
  diagnostics, reset reason, WiFi MAC, battery diagnostics, and serial logs.
- WQN cloud connection: optional WiFi station mode, pairing token storage,
  masked token logging, due problem sync, problem index refresh, review upload
  retry, Todo sync, and AI request upload.
- E-paper UI: local device pages for home/time, countdown, pomodoro, cached
  problems, Todo, word/notebook-facing study flows, and AI conversation status.
- E-paper refresh control: full refresh and local partial-window refresh support
  with cooldown and idle power-off controls.
- AI audio path: long-press confirm to record, release to upload 16 kHz mono PCM
  to the WQN server, then display transcript, reply text, and action summaries.
- Local storage: NVS holds only small control state (pairing/WiFi credentials,
  revisions, cursors and settings). Durable content lives on SPIFFS; the problem
  cache uses the versioned, block-compressed WQPC format with atomic temp/rename
  commits, while PSRAM is used only for volatile decode/UI snapshots.

## Security Boundary

The firmware must not contain Supabase service keys, DashScope keys, model
provider keys, notebook access rules, or AI function-call execution logic. It
authenticates to the WQN server with the device pairing token and treats HTTP
`401` as the only condition that clears the local token. Other server/provider
errors are displayed to the user without destroying pairing state.

Local `sdkconfig` may contain WiFi credentials for development and is ignored by
git. Do not commit copied firmware images, WiFi credentials, access tokens,
audio captures, or user data.

## Build

Use ESP-IDF 5.5.4 for this project. The primary WSL environment is:

```txt
/home/unknow/esp/esp-idf-v5.5
```

Build from WSL with:

```bash
cd /home/unknow/projects/firmware/firmware/wqn-zectrix-note4
source /home/unknow/esp/esp-idf-v5.5/export.sh
idf.py -B build-ai-local-s3 set-target esp32s3
idf.py -B build-ai-local-s3 build
```

The previous Windows checkout remains available for recovery/reference at
`D:\projects\wqn-zectrix-note4-firmware`. Its ESP-IDF root is:

```txt
D:\Program\Espressif\frameworks\esp-idf-v5.5.4
```

The default WQN ESP32 API base is:

```txt
https://wqn.helema.cn/api/esp32
```

Development builds can override it:

```bash
idf.py -B build-ai-local-s3 -DWQN_API_BASE=https://your-host.example.com/api/esp32 build
```

## Word reference baseline

- [Product and runtime baseline](WORD_BASELINE.md)
- [StudySession lifecycle](STUDY_SESSION_PROTOCOL.md)
- [Deterministic Word Pack v2](WORD_PACK_V2.md)
- [Durable offline outbox](WORD_OFFLINE_OUTBOX.md)
- [Word cutover checklist](WORD_RELEASE_CHECKLIST.md)
- [Reuse guide for problems and notes](STUDY_RUNTIME_REUSE.md)

## Main Configuration Switches

Configure features through `idf.py menuconfig` under `WQN firmware`:

- `CONFIG_WQN_WIFI_STA_ENABLE`: enables WiFi station mode and WQN API calls.
- `CONFIG_WQN_WIFI_SSID` / `CONFIG_WQN_WIFI_PASSWORD`: local development WiFi
  credentials stored in ignored `sdkconfig`.
- `CONFIG_WQN_EPD_UI_ENABLE`: enables the e-paper UI and button-driven device
  application.
- `CONFIG_WQN_EPD_LOCAL_PARTIAL_ENABLE`: enables the Note4 local partial-window
  refresh path.
- `CONFIG_WQN_EPD_IDLE_POWER_OFF_MS`: powers off the e-paper rail after UI idle
  time to save battery.
- `CONFIG_WQN_DEEP_SLEEP_ENABLE`: optional experimental deep sleep path.
- `CONFIG_WQN_AI_ENABLE`: enables AI firmware modules; provider secrets still
  stay server-side.
- `CONFIG_WQN_AI_AUDIO_SELFTEST_ENABLE`: captures and logs audio statistics at boot
  without uploading audio.

## Local Flashing

The local flashing helper builds and flashes the development device:

```cmd
firmware\wqn-zectrix-note4\tools\portable-flasher\build-flash-local.bat COM7
```

Defaults:

- Build directory: `build-ai-local-s3`
- Port: `COM7`
- Baud: `921600`

`COM5` is treated as the official-firmware checkpoint and the helper refuses to
flash it. Use `COM7` for development unless the target device assignment has
explicitly changed.

## Recovery Reference

Keep the verified official backup image outside this repository:

```txt
D:\projects\ESP32DOC\zectrix_note4_backup.bin
```

Known backup facts:

- Expected size: `16777216` bytes / 16 MiB.
- SHA256: `C965245CE42F90938A28588D88A0DBFF9D03E2AD76C31B27DE65B03875FD1F02`.

Command templates:

```powershell
# Read a full flash backup from a device.
python -m esptool --chip esp32s3 -p COMx -b 460800 read_flash 0x0 0x1000000 current_device_backup.bin

# Restore the preserved official backup.
python -m esptool --chip esp32s3 -p COMx -b 460800 write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 D:\projects\ESP32DOC\zectrix_note4_backup.bin
```

## Development Notes

Current runtime and release references:

- [Architecture and resource ownership](ARCHITECTURE.md)
- [Coordinated firmware/cloud release checklist](RELEASE_CHECKLIST.md)
- [Firmware troubleshooting](TROUBLESHOOTING.md)

- Keep firmware-side changes focused on device behavior, transport contracts,
  power control, local rendering, and local caches.
- Keep cloud-side logic in the WQN web service: ASR, model selection, notebook
  permissions, Todo tools, Supabase access, and AI function calls.
- Prefer fresh build directories for major feature lines (`build-ai-local-s3`,
  `build-user-s3`, or another explicit name) to avoid stale configuration.
- Serial logs and reverse-engineering extraction artifacts are local diagnostics
  and are ignored by git.
