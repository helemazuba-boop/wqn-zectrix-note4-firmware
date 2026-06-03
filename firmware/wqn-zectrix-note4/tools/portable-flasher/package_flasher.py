#!/usr/bin/env python3
"""Create and optionally upload a no-ESP-IDF Windows flasher bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import zipfile
from datetime import datetime, timezone
from pathlib import Path


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd: list[str], *, input_text: str | None = None) -> subprocess.CompletedProcess:
    print("+", " ".join(str(x) for x in cmd))
    return subprocess.run(cmd, input=input_text, text=True, check=True)


def capture(cmd: list[str], cwd: Path) -> str:
    try:
        return subprocess.check_output(cmd, cwd=str(cwd), text=True, stderr=subprocess.DEVNULL).strip()
    except Exception:
        return ""


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\r\n")


def make_update_bat(app_name: str, flash_mode: str, flash_freq: str, flash_size: str) -> str:
    return f"""@echo off
setlocal EnableExtensions
cd /d "%~dp0"

if "%~1"=="" (
  set /p PORT=Enter COM port, for example COM7: 
) else (
  set "PORT=%~1"
)

if "%PORT%"=="" (
  echo ERROR: COM port is required.
  pause
  exit /b 1
)

echo Updating WQN Note4 app on %PORT%.
echo This writes only the app partition at 0x20000 and preserves NVS pairing/WiFi data.
call :flash_app 921600
if errorlevel 1 (
  echo High-speed flash failed. Retrying at 460800...
  call :flash_app 460800
)
if errorlevel 1 (
  echo Flash failed.
  pause
  exit /b 1
)

echo Flash complete. The device will reset now.
pause
exit /b 0

:flash_app
tools\\esptool.exe --chip esp32s3 --port "%PORT%" --baud %1 --before default_reset --after hard_reset --no-stub write_flash --flash_mode {flash_mode} --flash_freq {flash_freq} --flash_size {flash_size} 0x20000 "firmware\\{app_name}"
exit /b %errorlevel%
"""


def make_factory_bat(files: dict[str, str], flash_mode: str, flash_freq: str, flash_size: str) -> str:
    ordered = sorted(files.items(), key=lambda item: int(item[0], 16))
    pairs = " ".join(f'{offset} "firmware\\{Path(rel).name}"' for offset, rel in ordered)
    return f"""@echo off
setlocal EnableExtensions
cd /d "%~dp0"

echo WQN Note4 core partition restore.
echo This writes bootloader, partition table, OTA data, and app.
echo It does not erase the full flash, so NVS pairing/WiFi data is normally preserved.
echo Confirm the target COM port before continuing.
set /p CONFIRM=Type FACTORY to continue: 
if /I not "%CONFIRM%"=="FACTORY" (
  echo Cancelled.
  pause
  exit /b 1
)

if "%~1"=="" (
  set /p PORT=Enter COM port, for example COM7: 
) else (
  set "PORT=%~1"
)

if "%PORT%"=="" (
  echo ERROR: COM port is required.
  pause
  exit /b 1
)

call :flash_factory 921600
if errorlevel 1 (
  echo High-speed flash failed. Retrying at 460800...
  call :flash_factory 460800
)
if errorlevel 1 (
  echo Flash failed.
  pause
  exit /b 1
)

echo Flash complete. The device will reset now.
pause
exit /b 0

:flash_factory
tools\\esptool.exe --chip esp32s3 --port "%PORT%" --baud %1 --before default_reset --after hard_reset --no-stub write_flash --flash_mode {flash_mode} --flash_freq {flash_freq} --flash_size {flash_size} {pairs}
exit /b %errorlevel%
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build-ai-local-s3")
    parser.add_argument("--output-dir", default="dist")
    parser.add_argument("--esptool-exe", required=True)
    parser.add_argument("--ssh-host", default="aliyun")
    parser.add_argument("--remote-dir", default="/www/wwwroot/alist_storage/WQN Deck")
    parser.add_argument("--upload", action="store_true")
    args = parser.parse_args()

    project = Path(__file__).resolve().parents[2]
    build_dir = (project / args.build_dir).resolve()
    output_dir = (project / args.output_dir).resolve()
    esptool_exe = Path(args.esptool_exe).resolve()

    flasher_args_path = build_dir / "flasher_args.json"
    if not flasher_args_path.exists():
        raise SystemExit(f"Missing build flasher_args.json: {flasher_args_path}")
    if not esptool_exe.exists():
        raise SystemExit(f"Missing portable esptool: {esptool_exe}")

    flasher = json.loads(flasher_args_path.read_text(encoding="utf-8"))
    flash_settings = flasher.get("flash_settings", {})
    flash_files = flasher.get("flash_files", {})
    app = flasher.get("app", {})
    app_file = app.get("file")
    if not flash_files or not app_file:
        raise SystemExit("flasher_args.json does not contain expected flash_files/app entries")

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    commit = capture(["git", "rev-parse", "--short", "HEAD"], project) or "nogit"
    dirty = bool(capture(["git", "status", "--short"], project))
    package_name = f"WQN-Note4-Flasher-{stamp}-{commit}"
    package_root = output_dir / package_name
    if package_root.exists():
        shutil.rmtree(package_root)
    (package_root / "firmware").mkdir(parents=True)
    (package_root / "tools").mkdir(parents=True)

    shutil.copy2(esptool_exe, package_root / "tools" / "esptool.exe")

    copied: dict[str, str] = {}
    for offset, rel in flash_files.items():
        src = build_dir / rel
        if not src.exists():
            raise SystemExit(f"Missing flash artifact at {offset}: {src}")
        dst = package_root / "firmware" / Path(rel).name
        shutil.copy2(src, dst)
        copied[offset] = dst.name

    manifest = {
        "package": package_name,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "git_commit": commit,
        "git_dirty": dirty,
        "chip": flasher.get("extra_esptool_args", {}).get("chip", "esp32s3"),
        "flash_settings": flash_settings,
        "app_only_update": {
            "offset": app.get("offset", "0x20000"),
            "file": Path(app_file).name,
        },
        "factory_restore": {
            "erases_full_flash": False,
            "preserves_nvs_by_default": True,
            "files": copied,
        },
        "artifacts": [],
    }

    for offset, name in sorted(copied.items(), key=lambda item: int(item[0], 16)):
        p = package_root / "firmware" / name
        manifest["artifacts"].append(
            {
                "offset": offset,
                "file": f"firmware/{name}",
                "size": p.stat().st_size,
                "sha256": sha256_file(p),
            }
        )
    manifest["artifacts"].append(
        {
            "file": "tools/esptool.exe",
            "size": (package_root / "tools" / "esptool.exe").stat().st_size,
            "sha256": sha256_file(package_root / "tools" / "esptool.exe"),
        }
    )

    flash_mode = flash_settings.get("flash_mode", "dio")
    flash_freq = flash_settings.get("flash_freq", "80m")
    flash_size = flash_settings.get("flash_size", "16MB")
    app_name = Path(app_file).name

    write_text(package_root / "flash_update.bat", make_update_bat(app_name, flash_mode, flash_freq, flash_size))
    write_text(package_root / "flash_factory.bat", make_factory_bat(copied, flash_mode, flash_freq, flash_size))
    write_text(
        package_root / "README.txt",
        f"""WQN Note4 portable Windows flasher

Package: {package_name}
Chip: ESP32-S3
Flash: {flash_mode} / {flash_freq} / {flash_size}

Use on a Windows computer without ESP-IDF or Python:

1. Extract this zip.
2. Connect the device over USB.
3. Check the COM port in Device Manager.
4. Run one of:

   flash_update.bat COM7
   flash_factory.bat COM7

flash_update.bat:
  Writes only firmware/{app_name} to app offset 0x20000.
  This is the normal update path and preserves NVS pairing/WiFi data.

flash_factory.bat:
  Writes bootloader, partition table, OTA data, and app.
  It asks for FACTORY confirmation.
  It does not erase the full flash, so NVS is normally preserved.

If 921600 baud fails, the scripts retry at 460800 baud automatically.
The scripts use esptool --no-stub for maximum portability across packaged builds.
""",
    )
    write_text(package_root / "manifest.json", json.dumps(manifest, indent=2, ensure_ascii=False))

    output_dir.mkdir(parents=True, exist_ok=True)
    zip_path = output_dir / f"{package_name}.zip"
    if zip_path.exists():
        zip_path.unlink()
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for path in package_root.rglob("*"):
            if path.is_file():
                zf.write(path, path.relative_to(package_root.parent))

    zip_hash = sha256_file(zip_path)
    sha_path = output_dir / f"{zip_path.name}.sha256"
    write_text(sha_path, f"{zip_hash}  {zip_path.name}\n")

    print(f"Package directory: {package_root}")
    print(f"Package zip: {zip_path}")
    print(f"SHA256: {zip_hash}")

    if args.upload:
        remote_file = args.remote_dir.rstrip("/") + "/" + zip_path.name
        remote_sha_file = remote_file + ".sha256"
        run(["ssh", args.ssh_host, f"mkdir -p {shlex.quote(args.remote_dir)}"])
        sftp_batch = (
            f'put "{zip_path.as_posix()}" "{remote_file}"\n'
            f'put "{sha_path.as_posix()}" "{remote_sha_file}"\n'
        )
        run(["sftp", "-b", "-", args.ssh_host], input_text=sftp_batch)
        remote = subprocess.check_output(
            ["ssh", args.ssh_host, f"sha256sum {shlex.quote(remote_file)}"],
            text=True,
        ).strip()
        print(f"Remote SHA256: {remote}")
        if not remote.startswith(zip_hash):
            raise SystemExit("Remote SHA256 does not match local package")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
