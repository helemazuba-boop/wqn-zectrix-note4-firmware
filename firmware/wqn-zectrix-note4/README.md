# WQN ZecTrix Note4 Firmware

This is the headless ESP-IDF firmware skeleton for the WQN ZecTrix Note4
companion device. The default build is offline: it initializes safe board pins,
prints diagnostics over Serial, parses local API fixtures, and does not start
WiFi or call any WQN network endpoint.

## Current Scope

- ESP-IDF project targeting `esp32s3`.
- 16 MB Flash layout aligned with the official ZecTrix baseline.
- Safe GPIO initialization for `zectrix-s3-epaper-4.2`: GPIO17 VBAT hold is on,
  while EPD, audio, amp, and NFC power stay off.
- Boot diagnostics: firmware version, board id, target, ESP-IDF version, chip
  features, Flash size, PSRAM initialized/size, WiFi MAC, and reset reason.
- Optional WiFi station flow behind `CONFIG_WQN_WIFI_STA_ENABLE`.
- No AP provisioning, pairing, sync, review upload, or HTTP requests yet.

Do not flash this firmware to the physical device until `CMOS5` and the backup
restore path are confirmed.

## Build

Activate ESP-IDF first, then run:

```powershell
cd D:\projects\wqn-zectrix-note4-firmware\firmware\wqn-zectrix-note4
idf.py --no-ccache -B build-user-s3 set-target esp32s3
idf.py --no-ccache -B build-user-s3 build
```

The Codex sandbox has a separate Windows user and can fail ESP-IDF toolchain
discovery. The verified local command path is:

```cmd
set IDF_TOOLS_PATH=D:\Program\Espressif
set IDF_PATH=D:\Program\Espressif\frameworks\esp-idf-v5.5.4
call D:\Program\Espressif\idf_cmd_init.bat
idf.py --no-ccache -B build-user-s3 build
```

Development builds may override the API base for future work:

```powershell
idf.py -DWQN_API_BASE=https://your-host.example.com/api/esp32 build
```

## Optional WiFi Station

WiFi is disabled by default. To test station mode after the board/recovery
checks are complete:

```powershell
idf.py -B build-user-s3 menuconfig
```

Enable:

```txt
WQN firmware -> Enable WQN WiFi station
WQN firmware -> WQN WiFi SSID
WQN firmware -> WQN WiFi password
```

The credentials are stored in local `sdkconfig`, which is ignored by git. When
enabled, the firmware starts STA mode, logs connect/disconnect events, prints
IP/netmask/gateway after DHCP succeeds, and retries after disconnects. It still
does not call WQN APIs.

## Recovery Gate

Reference material:

```txt
D:\projects\ESP32DOC\zectrix
D:\projects\ESP32DOC\zectrix\main\boards\zectrix-s3-epaper-4.2
D:\projects\ESP32DOC\zectrix_note4_backup.bin
```

Known backup facts:

- Path: `D:\projects\ESP32DOC\zectrix_note4_backup.bin`
- Expected size: `16777216` bytes / 16 MiB
- SHA256: `C965245CE42F90938A28588D88A0DBFF9D03E2AD76C31B27DE65B03875FD1F02`

Before any custom flash, confirm whether `CMOS5` means a different chip target
or only an official firmware/runtime label. Also verify the restore flow on the
actual device. Suggested command templates, to be run only after the target and
serial port are confirmed:

```powershell
# Read current full flash backup from the device.
python -m esptool --chip esp32s3 -p COMx -b 460800 read_flash 0x0 0x1000000 current_device_backup.bin

# Restore the preserved official backup.
python -m esptool --chip esp32s3 -p COMx -b 460800 write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 D:\projects\ESP32DOC\zectrix_note4_backup.bin
```

Keep the official backup untouched and do not commit copied firmware images,
WiFi credentials, tokens, or user data.
