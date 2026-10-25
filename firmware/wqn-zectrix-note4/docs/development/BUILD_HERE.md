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
└── docs/development/BUILD_HERE.md # this file
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
cd <repo-root>\firmware\wqn-zectrix-note4
call "<esp-idf-dir>\export.bat"
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

Use the [reliability acceptance checklist](../engineering/FIRMWARE_RELIABILITY_ACCEPTANCE.md)
to choose automated, stage-device and release checks by impact. Historical batch
device matrices are retained evidence, not repeated per-patch instructions.

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

## WSL B5 diagnostic comparison (2026-10-23)

The active WSL build remains `build-wsl` with ESP-IDF 5.5.6. The one-purpose
control directory `build-b5-diag-off-20261023` is an explicit exception to the
single-directory rule: it uses its own sdkconfig with RAM diagnostics disabled.
The normal root sdkconfig and `build-wsl` stay diagnostics-on.

After loading this checkout's IDF environment, run
`python3 scripts/build_diagnostic_pair.py --output dist/<new-artifact-directory>`
from the firmware root on a clean tracked worktree. The tool **fullcleans the
default build output**, rebuilds both profiles with the same build timestamp,
checks the full config delta and artifact identities, and archives both without
flashing. The control directory and output directory must be new; it refuses
to overwrite an existing pair. Config archives may contain local WiFi credentials
and stay private and ignored. Only the RAM diagnostic switch and dependent
options may differ. Existing API/WS/build-type overrides are carried over.

The verified pair is in `dist/b5-diagnostic-pair-20261023-clean/manifest.json`;
the first attempt in `dist/b5-diagnostic-pair-20261023/` is superseded.
See [batch 12](../engineering/RELIABILITY_BATCH_12.md) for hashes, the build
gate correction, storage collection, and device measurements. To compare images
on hardware, preserve NVS/SPIFFS with the existing app-only flashing procedure;
an entry point that erases the whole chip destroys the comparison conditions.
Neither image was flashed during pair preparation.

Batch 13 adds a runtime fix, so its current matched pair is
`dist/b5-diagnostic-pair-b13-20261023/manifest.json`, baked build `ab8abed8`.
Its dedicated control directory is `build-b5-diag-off-b13-20261023`, another
explicit B5 comparison output; the batch 12 artifacts remain historical.
The preparation command was
`python3 scripts/build_diagnostic_pair.py --control-build build-b5-diag-off-b13-20261023 --output dist/b5-diagnostic-pair-b13-20261023`.
For a later pair, choose new `--control-build build-<purpose>-<YYYYMMDD>` and
`--output` paths. Do not compare one old-runtime image with one new-runtime
image as a RAM-diagnostics experiment. See [batch 13](../engineering/RELIABILITY_BATCH_13.md)
for the active-file protection and device matrix.

## Reliability worktree, batch 14 (2026-10-24)

The main checkout is on `fix/cross-repo-security-20261023`. Reliability work
continues on `fix/firmware-reliability` in
`/home/unknow/projects/firmware/.worktrees/reliability-20261024`, retaining
the existing `a565923` security commit. This worktree has its own private
sdkconfig and `build-wsl`; the main checkout's build output was not reused.

From this worktree's firmware directory, the verified pair was prepared with
`python3 scripts/build_diagnostic_pair.py --control-build build-b5-diag-off-b14-20261024 --output dist/b5-diagnostic-pair-b14-20261024`.
The dated control directory is an explicit B5 comparison output. Its manifest
is at `dist/b5-diagnostic-pair-b14-20261024/manifest.json`, within the worktree.
Use that archived pair to evaluate batch 14. Building from the main security checkout
does not include the new reliability commit. See
[batch 14](../engineering/RELIABILITY_BATCH_14.md) for identity and device checks.

Batch 15 continued in the same worktree. Its archived runtime pair is
`dist/b5-diagnostic-pair-b15-20261024/manifest.json`, prepared with
`--control-build build-b5-diag-off-b15-20261024 --output dist/b5-diagnostic-pair-b15-20261024`.
This dated control directory is another explicit B5 comparison output; earlier
pairs remain historical. Use the same-source batch 15 images when evaluating
the pack callback change. See [batch 15](../engineering/RELIABILITY_BATCH_15.md)
for identity, resource costs and device measurements.

Batch 16's archived runtime pair is
`dist/b5-diagnostic-pair-b16-20261024/manifest.json` in the same reliability
worktree. Its dedicated control build was prepared with
`--control-build build-b5-diag-off-b16-20261024 --output dist/b5-diagnostic-pair-b16-20261024`.
The dated directory is an explicit B5 comparison output. Use this same-source
pair for path admission/cache-reset validation; prior pairs remain historical.
See [batch 16](../engineering/RELIABILITY_BATCH_16.md).

Batch 17's archived runtime pair is
`dist/b5-diagnostic-pair-b17-20261025/manifest.json` in the reliability worktree.
Its dedicated control build was prepared with
`--control-build build-b5-diag-off-b17-20261025 --output dist/b5-diagnostic-pair-b17-20261025`.
The dated directory is an explicit B5 comparison output. Use this same-source
pair for backup retention/pruning validation; earlier pairs remain historical.
See [batch 17](../engineering/RELIABILITY_BATCH_17.md) for artifact identity and device checks.

Batch 18's archived runtime pair is
`dist/b5-diagnostic-pair-b18-20261025/manifest.json` in the reliability worktree.
Its dedicated control build was prepared with
`--control-build build-b5-diag-off-b18-20261025 --output dist/b5-diagnostic-pair-b18-20261025`.
Use this same-source pair for manifest encoder-failure validation; earlier pairs
remain historical. The latest observed serial identity is `a565923a-dirty`, so
verify the candidate build label before attributing device results to this batch.
See [batch 18](../engineering/RELIABILITY_BATCH_18.md) for identity and device checks.

With device acceptance deferred, batch 19 updates the default diagnostic-on
`build-wsl` only. Its current artifact manifest is
`dist/reliability-b19-20261025/manifest.json` in the reliability worktree,
prepared after `idf.py -B build-wsl reconfigure build` at runtime commit `c4bc7ec`.
This is a single build; a new diagnostic-off control was not prepared. Generate
a matching pair for the eventual joint power comparison. See
[batch 19](../engineering/RELIABILITY_BATCH_19.md) for the code queue and boundaries.

## History

- 2026-06-14: Removed 15 stale `build-*` dirs; moved 13 `.bat` files to `scripts/`;
  archived 15 `*.log` / `*.err` to `logs/archive-2026-06-14/`. Wrote this file.
