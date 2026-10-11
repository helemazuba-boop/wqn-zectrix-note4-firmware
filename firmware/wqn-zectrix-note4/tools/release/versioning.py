#!/usr/bin/env python3
"""Release identity: source fingerprint, version numbering, diff persistence.

The version is derived from file *content*, not from commits or tags, because
this project ships from dirty working trees several times a day. Two builds of
the same content always get the same version; any real change gets a new one.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import tarfile
import tempfile
from datetime import datetime, timezone
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parents[2]
CACHE_DIR = PROJECT_DIR / ".cache"
VERSION_INDEX_PATH = CACHE_DIR / "version-index.json"
IDENTITY_ENV_NAME = "wqn-release-identity.env"

DEFAULT_BUILD_DIR = "build-ai-local-s3"

# Ignored by git but still a build input: without it a menuconfig change would
# produce a different firmware with an unchanged version.
EXTRA_BUILD_INPUTS = ("sdkconfig",)

MANIFEST_EXCLUDE_SUFFIX = (".log", ".bin", ".elf", ".map", ".lock", ".tmp", ".tgz", ".zip")
MANIFEST_EXCLUDE_DIRS = {
    "dist",
    "backups",
    "logs",
    ".cache",
    ".pyinstaller",
    "__pycache__",
    "docs",
    # Release tooling drives the version; it must not bump the version itself.
    "tools",
}

SNAPSHOT_DIRS = ("main", "components", "partitions", "cmake", "tools")
SNAPSHOT_FILES = (
    "CMakeLists.txt",
    "sdkconfig",
    "sdkconfig.defaults",
    "sdkconfig.ai-local.defaults",
    "sdkconfig.defaults.esp32s3",
    "sdkconfig.audio-selftest.defaults",
)

MISSING = "<missing>"


# --------------------------------------------------------------------------
# git helpers
# --------------------------------------------------------------------------

_repo_root: Path | None = None


def repo_root() -> Path:
    global _repo_root
    if _repo_root is None:
        out = subprocess.run(
            ["git", "-C", str(PROJECT_DIR), "rev-parse", "--show-toplevel"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if out.returncode != 0:
            raise RuntimeError(f"not a git repository: {PROJECT_DIR}")
        _repo_root = Path(out.stdout.strip())
    return _repo_root


def project_relpath() -> str:
    return PROJECT_DIR.relative_to(repo_root()).as_posix()


def git(*args: str, env: dict[str, str] | None = None, check: bool = True) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=str(repo_root()),
        env=env or os.environ,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if check and result.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def base_commit() -> str:
    out = git("rev-parse", "HEAD", check=False).strip()
    return out or "nogit"


def is_dirty() -> bool:
    return bool(git("status", "--short", "--", project_relpath(), check=False).strip())


def changed_file_count() -> tuple[int, int]:
    """(modified_or_deleted, untracked) inside the project directory."""
    rel = project_relpath()
    modified = [
        line
        for line in git("status", "--short", "--", rel, check=False).splitlines()
        if line.strip() and not line.startswith("??")
    ]
    untracked = [
        line
        for line in git("status", "--short", "--", rel, check=False).splitlines()
        if line.startswith("??")
    ]
    return len(modified), len(untracked)


# --------------------------------------------------------------------------
# content hashing
# --------------------------------------------------------------------------


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _excluded(rel_path: str) -> bool:
    parts = rel_path.split("/")
    if any(p in MANIFEST_EXCLUDE_DIRS or p.startswith("build-") or p == "build" for p in parts):
        return True
    return rel_path.endswith(MANIFEST_EXCLUDE_SUFFIX)


def source_manifest() -> dict[str, str]:
    """Map of project-relative path -> sha256 for every file that can change the build."""
    rel = project_relpath()
    root = repo_root()
    entries: set[str] = set()
    for args in (
        ("ls-files", "-z", "--", rel),
        ("ls-files", "-z", "--others", "--exclude-standard", "--", rel),
    ):
        entries.update(p for p in git(*args, check=False).split("\0") if p)

    manifest: dict[str, str] = {}
    for path in sorted(entries):
        project_rel = Path(path).relative_to(rel).as_posix()
        if _excluded(project_rel):
            continue
        abs_path = root / path
        manifest[project_rel] = sha256_file(abs_path) if abs_path.is_file() else MISSING
    for name in EXTRA_BUILD_INPUTS:
        candidate = PROJECT_DIR / name
        if candidate.is_file():
            manifest[name] = sha256_file(candidate)
    return manifest


def idf_version() -> str:
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        return "unknown"
    version_cmake = Path(idf_path) / "tools" / "cmake" / "version.cmake"
    if not version_cmake.is_file():
        return "unknown"
    text = version_cmake.read_text(encoding="utf-8", errors="replace")
    parts = [
        re.search(rf"set\(IDF_VERSION_{name}\s+(\d+)\)", text)
        for name in ("MAJOR", "MINOR", "PATCH")
    ]
    if not all(parts):
        return "unknown"
    return ".".join(m.group(1) for m in parts if m)


def fingerprint(manifest: dict[str, str], profile: str, idf: str) -> str:
    h = hashlib.sha256()
    for key in sorted(manifest):
        h.update(key.encode("utf-8"))
        h.update(manifest[key].encode("utf-8"))
    h.update(profile.encode("utf-8"))
    h.update(idf.encode("utf-8"))
    return h.hexdigest()


def profile_from_build_dir(build_dir: str | None) -> str:
    name = Path(build_dir or DEFAULT_BUILD_DIR).name
    return name[len("build-") :] if name.startswith("build-") else name


# --------------------------------------------------------------------------
# version numbering
# --------------------------------------------------------------------------


def load_version_index() -> dict:
    if VERSION_INDEX_PATH.is_file():
        return json.loads(VERSION_INDEX_PATH.read_text(encoding="utf-8"))
    return {"days": {}}


def save_version_index(index: dict) -> None:
    VERSION_INDEX_PATH.parent.mkdir(parents=True, exist_ok=True)
    VERSION_INDEX_PATH.write_text(
        json.dumps(index, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )


def ordered_versions(index: dict) -> list[str]:
    pairs = []
    for date, day in index.get("days", {}).items():
        for serial in day:
            if serial.isdigit():
                pairs.append((date, int(serial), f"{date}.{serial}"))
    pairs.sort()
    return [version for _, _, version in pairs]


def assign_version(
    fingerprint: str,
    *,
    base_commit: str,
    dirty: bool,
    kind: str,
    date: str | None = None,
    remote_max_serial: int = 0,
) -> tuple[str, bool]:
    """Return (version, reused). Reuses the serial when the fingerprint is known."""
    date = date or datetime.now().strftime("%Y%m%d")
    index = load_version_index()
    day = index.setdefault("days", {}).setdefault(date, {})

    for serial, record in day.items():
        if not serial.isdigit() or record.get("fingerprint") != fingerprint:
            continue
        if kind == "release" and record.get("kind") != "release":
            record["kind"] = "release"
            save_version_index(index)
        return f"{date}.{serial}", True

    used = [int(s) for s in day if s.isdigit()]
    serial = str(max([*used, remote_max_serial], default=0) + 1)
    day[serial] = {
        "fingerprint": fingerprint,
        "base_commit": base_commit,
        "dirty": dirty,
        "kind": kind,
        "created_at": utc_now_iso(),
    }
    save_version_index(index)
    return f"{date}.{serial}", False


def record_from(index: dict, version: str) -> dict | None:
    """Look up a version's record inside any index of this shape."""
    date, _, serial = version.rpartition(".")
    if not date or not serial.isdigit():
        return None
    return index.get("days", {}).get(date, {}).get(serial)


def version_record(version: str) -> dict | None:
    """Look up the local index record for a version, e.g. 20260907.3."""
    return record_from(load_version_index(), version)


# --------------------------------------------------------------------------
# diff persistence
# --------------------------------------------------------------------------


def diff_manifests(old: dict[str, str], new: dict[str, str]) -> list[tuple[str, str]]:
    """Return [(status, path)] with status in A/M/D."""
    changes: list[tuple[str, str]] = []
    for path in sorted(set(old) | set(new)):
        before, after = old.get(path), new.get(path)
        if before == after:
            continue
        if before is None or before == MISSING:
            changes.append(("A", path))
        elif after is None or after == MISSING:
            changes.append(("D", path))
        else:
            changes.append(("M", path))
    return changes


def write_changes_patch(dest: Path) -> None:
    """Full working-tree diff vs HEAD, including untracked files.

    Uses a throwaway GIT_INDEX_FILE so the real index is never touched.
    """
    rel = project_relpath()
    tmp_index = tempfile.mktemp(prefix="wqn-release-index-")
    env = {**os.environ, "GIT_INDEX_FILE": tmp_index}
    try:
        git("read-tree", "HEAD", env=env)
        git("add", "-A", "--", rel, env=env)
        patch = git("diff", "--cached", "HEAD", "--", rel, env=env, check=False)
    finally:
        if os.path.exists(tmp_index):
            os.unlink(tmp_index)
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(patch, encoding="utf-8")


def write_source_snapshot(dest: Path) -> None:
    def keep(info: tarfile.TarInfo) -> tarfile.TarInfo | None:
        parts = Path(info.name).parts
        if any(
            p in MANIFEST_EXCLUDE_DIRS or p.startswith("build-") or p == "build" for p in parts
        ):
            return None
        if info.isfile() and info.name.endswith((".log", ".bin", ".elf", ".map")):
            return None
        return info

    dest.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(dest, "w:gz") as tar:
        for name in SNAPSHOT_DIRS:
            path = PROJECT_DIR / name
            if path.is_dir():
                tar.add(path, arcname=name, filter=keep)
        for name in SNAPSHOT_FILES:
            path = PROJECT_DIR / name
            if path.is_file():
                tar.add(path, arcname=name)


def recent_commits(since_commit: str, limit: int = 10) -> list[str]:
    if since_commit in ("", "nogit"):
        return []
    out = git(
        "log",
        "--no-merges",
        "--pretty=format:%h %s",
        "-n",
        str(limit),
        f"{since_commit}..HEAD",
        "--",
        project_relpath(),
        check=False,
    )
    return [line for line in out.splitlines() if line.strip()]


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()
