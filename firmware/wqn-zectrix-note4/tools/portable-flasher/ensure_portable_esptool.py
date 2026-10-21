#!/usr/bin/env python3
"""Resolve the pinned esptool.exe used by the portable Windows flasher bundle.

The bundle must work on machines with no Python and no ESP-IDF, so it ships a
standalone Windows executable. PyInstaller cannot cross-build a Windows PE from
WSL (and the old hand-rolled copy was never reproducible), so instead of
rebuilding we pin the official prebuilt release in esptool.lock.json and resolve
it on demand:

  1. the local .cache copy, when it already matches the lock
  2. the mirror on the release host (fast, no GitHub dependency)
  3. the official release zip
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
LOCK_PATH = TOOLS_DIR / "esptool.lock.json"

DEFAULT_SSH_HOST = "aliyun"
DEFAULT_REMOTE_DIR = "/www/wwwroot/alist_storage/WQN NOTE 4"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def load_lock() -> dict:
    if not LOCK_PATH.is_file():
        raise SystemExit(f"Missing esptool lock file: {LOCK_PATH}")
    return json.loads(LOCK_PATH.read_text(encoding="utf-8"))


def matches(path: Path, lock: dict) -> bool:
    return (
        path.is_file()
        and path.stat().st_size == lock["size"]
        and sha256_file(path) == lock["sha256"]
    )


def try_mirror(dest: Path, lock: dict, ssh_host: str, remote_dir: str) -> bool:
    remote = f'{remote_dir.rstrip("/")}/vendor/{lock["mirror_name"]}'
    print("+ scp", f"{ssh_host}:{remote}")
    result = subprocess.run(
        ["scp", "-q", f"{ssh_host}:{remote}", str(dest)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if result.returncode != 0:
        print("  mirror unavailable:", result.stdout.strip(), file=sys.stderr)
        return False
    if matches(dest, lock):
        return True
    print("  mirror contents did not match the lock", file=sys.stderr)
    return False


def try_official(dest: Path, lock: dict) -> bool:
    with tempfile.TemporaryDirectory() as tmp:
        zip_path = Path(tmp) / lock["asset"]
        print("+ download", lock["url"])
        with urllib.request.urlopen(lock["url"], timeout=600) as src, zip_path.open("wb") as dst:
            shutil.copyfileobj(src, dst)
        with zipfile.ZipFile(zip_path) as zf, zf.open(lock["member"]) as src, dest.open("wb") as out:
            shutil.copyfileobj(src, out)
    return matches(dest, lock)


def main() -> int:
    parser = argparse.ArgumentParser(description="Resolve the pinned portable esptool.exe")
    parser.add_argument("--output", default=str(TOOLS_DIR / ".cache" / "esptool.exe"))
    parser.add_argument("--force", action="store_true", help="Re-resolve even if the cache matches")
    parser.add_argument("--ssh-host", default=DEFAULT_SSH_HOST)
    parser.add_argument("--remote-dir", default=DEFAULT_REMOTE_DIR)
    args = parser.parse_args()

    output = Path(args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    lock = load_lock()

    if not args.force and matches(output, lock):
        print(f"Portable esptool up to date: {output} (v{lock['version']})")
        return 0

    for attempt in (
        lambda: try_mirror(output, lock, args.ssh_host, args.remote_dir),
        lambda: try_official(output, lock),
    ):
        if attempt():
            print(f"Portable esptool ready: {output} (v{lock['version']})")
            return 0
        output.unlink(missing_ok=True)

    print(
        f"Could not obtain esptool v{lock['version']}.\n"
        f"Download it manually and place it at {output}:\n"
        f"  {lock['url']}\n"
        f"  -> extract {lock['member']}",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
