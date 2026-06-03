#!/usr/bin/env python3
"""Build a standalone esptool.exe for the portable Windows flasher bundle."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    print("+", " ".join(str(x) for x in cmd))
    return subprocess.run(cmd, check=True, **kwargs)


def works(exe: Path) -> bool:
    if not exe.exists():
        return False
    try:
        result = subprocess.run(
            [str(exe), "version"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=30,
        )
    except Exception:
        return False
    print(result.stdout.strip())
    return result.returncode == 0 and "esptool" in result.stdout.lower()


def ensure_pyinstaller() -> None:
    try:
        import PyInstaller.__main__  # noqa: F401
        return
    except ImportError:
        pass

    print("PyInstaller is missing in the active Python environment; installing it now.")
    run([sys.executable, "-m", "pip", "install", "pyinstaller"])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, help="Output esptool.exe path")
    parser.add_argument("--force", action="store_true", help="Rebuild even if output works")
    args = parser.parse_args()

    output = Path(args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    if not args.force and works(output):
        print(f"Portable esptool already exists: {output}")
        return 0

    ensure_pyinstaller()

    cache_dir = Path(__file__).resolve().parent / ".pyinstaller"
    entry = cache_dir / "esptool_entry.py"
    work_dir = cache_dir / "work"
    spec_dir = cache_dir / "spec"
    cache_dir.mkdir(parents=True, exist_ok=True)
    entry.write_text(
        "import esptool\n"
        "\n"
        "if __name__ == '__main__':\n"
        "    esptool._main()\n",
        encoding="utf-8",
    )

    run(
        [
            sys.executable,
            "-m",
            "PyInstaller",
            "--clean",
            "--noconfirm",
            "--onefile",
            "--name",
            output.stem,
            "--distpath",
            str(output.parent),
            "--workpath",
            str(work_dir),
            "--specpath",
            str(spec_dir),
            "--collect-submodules",
            "esptool",
            "--collect-data",
            "esptool",
            "--collect-submodules",
            "serial",
            str(entry),
        ]
    )

    built = output.parent / f"{output.stem}.exe"
    if built != output and built.exists():
        built.replace(output)

    if not works(output):
        raise SystemExit(f"Built executable failed validation: {output}")

    print(f"Portable esptool ready: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
