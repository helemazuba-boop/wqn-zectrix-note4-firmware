# BUILD_HERE.md

> **WQN Zectrix Note4** firmware build & flash entry points.
> Last updated: 2026-06-14

## TL;DR

**Build dir:** `build-ai-local-s3` (only one in repo, kept on purpose)
**Flash entry:** `deploy.bat` in project root
**Helper scripts:** `scripts/`

```bat
:: Build + flash + monitor (from project root)
deploy.bat
```

If you only need one of those steps, see [Workflows](#workflows) below.

---

## Layout

```
firmware/wqn-zectrix-note4/
├── main/                       # application sources
├── partitions/                 # partition table(s)
├── tools/                      # python helpers (port detect, log parse, etc.)
├── scripts/                    # all .bat helpers (13 files)
│   ├── build-fix.bat
│   ├── check_chip.bat
│   ├── check_flash2.bat
│   ├── check_mode.bat
│   ├── check_monitor.bat
│   ├── erase_reflash.bat
│   ├── flash-corrected.bat
│   ├── flash-fix.bat
│   ├── flash_no_reset.bat
│   ├── monitor.bat
│   ├── read_flash.bat
│   ├── run_app.bat
│   └── verify_flash.bat
├── logs/                       # serial monitor logs (NOT committed)
│   └── archive-2026-06-14/     # historical snapshots
├── build-ai-local-s3/          # ONLY active build output
├── deploy.bat                  # ★ main entry: build + flash + monitor
├── sdkconfig                   # current ESP-IDF config
├── CMakeLists.txt
├── README.md
└── BUILD_HERE.md               # this file
```

---

## Workflows

### A. Build + flash + monitor (most common)

```bat
deploy.bat
```
- Prompts for COM port (default `COM7`).
- Runs `erase-flash` then `flash` then `monitor`.

### B. Build only

```bat
cd D:\projects\wqn-zectrix-note4-firmware\firmware\wqn-zectrix-note4
call "D:\Program\Espressif\frameworks\esp-idf-v5.5.4\export.bat"
idf.py -B build-ai-local-s3 build
```

### C. Monitor only

```bat
scripts\monitor.bat
```

The monitor applies IDF Monitor's `--no-reset` DTR/RTS ordering without needing
a Windows ESP-IDF/Python installation. This is mandatory on the ESP32-S3 native
USB-Serial-JTAG port: opening a resetting monitor produces
`USB_UART_CHIP_RESET (0x15)`, destroys RTC slow-memory diagnostics, and starts
Wi-Fi/display work that contaminates a sleep-efficiency observation.

### D. Recover from bad flash

```bat
scripts\erase_reflash.bat    :: full erase
scripts\flash-fix.bat        :: re-flash bootloader + app
```

---

## Rules of engagement (for AI / humans)

1. **One build dir.** Do NOT create a new `build-*` dir. Reuse `build-ai-local-s3`.
   If you absolutely need a new one, name it `build-<purpose>-<date>` and document it here.
2. **Scripts go in `scripts/`.** Do NOT drop new `.bat` files in project root.
3. **Logs go in `logs/`.** Do NOT commit `*.log` / `*.err` (see `.gitignore`).
4. **Editing `main/device_ui.cpp` (3906 lines):** prefer extracting a sub-module first,
   then editing. Do not add more inline logic to this file.
5. **Do NOT call `uart_write_bytes` in `main/`.** It collides with the console UART driver
   and breaks serial log output. Use `ESP_LOGx` instead.
6. **Observe first, change second.** A diagnostic should not require a reflash to confirm.

---

## History

- 2026-06-14: Removed 15 stale `build-*` dirs; moved 13 `.bat` files to `scripts/`;
  archived 15 `*.log` / `*.err` to `logs/archive-2026-06-14/`. Wrote this file.
