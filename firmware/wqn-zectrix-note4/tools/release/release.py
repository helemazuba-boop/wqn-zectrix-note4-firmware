#!/usr/bin/env python3
"""WQN Note4 firmware release CLI.

Every subcommand is driven by the content fingerprint, so the same working tree
always resolves to the same version and never consumes a new serial.
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
from datetime import datetime
from pathlib import Path

import versioning as V

HISTORY_NAME = "flash-history.jsonl"


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------


def resolve_build_dir(build_dir: str | None) -> Path:
    path = Path(build_dir or V.DEFAULT_BUILD_DIR)
    return path if path.is_absolute() else V.PROJECT_DIR / path


def dist_dir() -> Path:
    return V.PROJECT_DIR / "dist"


def release_dir(version: str) -> Path:
    return dist_dir() / version


def run(cmd: list[str], env: dict[str, str] | None = None, cwd: Path | None = None) -> None:
    print("+", " ".join(str(x) for x in cmd))
    subprocess.run(cmd, env=env, cwd=str(cwd or V.PROJECT_DIR), check=True)


def collect_identity(build_dir: str | None, kind: str, remote_max_serial: int = 0) -> dict:
    manifest = V.source_manifest()
    profile = V.profile_from_build_dir(build_dir)
    idf = V.idf_version()
    fp = V.fingerprint(manifest, profile, idf)
    base = V.base_commit()
    dirty = V.is_dirty()
    version, reused = V.assign_version(
        fp,
        base_commit=base,
        dirty=dirty,
        kind=kind,
        remote_max_serial=remote_max_serial,
    )
    # A reused version keeps its original timestamp so both machines report the
    # same build time for the same content.
    created_at = (V.version_record(version) or {}).get("created_at") or V.utc_now_iso()
    return {
        "version": version,
        "created_at": created_at,
        "fingerprint": fp,
        "base_commit": base,
        "base_commit_short": base[:8] if base != "nogit" else "nogit",
        "dirty": dirty,
        "profile": profile,
        "idf": idf,
        "kind": kind,
        "reused": reused,
        "file_count": len(manifest),
    }


def write_identity_env(build_dir: Path, info: dict) -> Path:
    build_dir.mkdir(parents=True, exist_ok=True)
    target = build_dir / V.IDENTITY_ENV_NAME
    target.write_text(
        f"WQN_RELEASE_VERSION={info['version']}\n"
        f"WQN_FINGERPRINT={info['fingerprint']}\n"
        f"WQN_BASE_COMMIT={info['base_commit']}\n"
        f"WQN_DIRTY={1 if info['dirty'] else 0}\n",
        encoding="utf-8",
    )
    return target


def build_env(info: dict) -> dict[str, str]:
    created = datetime.fromisoformat(info["created_at"]).astimezone()
    return {
        **os.environ,
        "WQN_RELEASE_VERSION": info["version"],
        "WQN_BUILD_TIME": created.strftime("%Y-%m-%d %H:%M"),
    }


# --------------------------------------------------------------------------
# identity
# --------------------------------------------------------------------------


def cmd_identity(args: argparse.Namespace) -> int:
    info = collect_identity(args.build_dir, args.kind)
    env_path = write_identity_env(resolve_build_dir(args.build_dir), info)

    if args.format == "export":
        print(f"export WQN_RELEASE_VERSION={info['version']}")
        print(f"export WQN_FINGERPRINT={info['fingerprint']}")
    else:
        print(json.dumps(info, indent=2, ensure_ascii=False))

    state = "dirty" if info["dirty"] else "clean"
    reused = " (reused)" if info["reused"] else ""
    print(
        f"WQN Note4 build identity: {info['version']}{reused} "
        f"[{state}, base {info['base_commit_short']}, "
        f"{info['file_count']} files, idf {info['idf']}] -> {env_path}",
        file=sys.stderr,
    )
    return 0


# --------------------------------------------------------------------------
# build
# --------------------------------------------------------------------------


def run_architecture_gate() -> None:
    """The M8 gate is an `all` target that never blocks compilation, so run it alone."""
    run(
        [
            "cmake",
            f"-DWQN_PROJECT_DIR={V.PROJECT_DIR}",
            "-P",
            str(V.PROJECT_DIR / "cmake" / "verify_architecture.cmake"),
        ]
    )


def cmd_build(args: argparse.Namespace) -> int:
    info = collect_identity(args.build_dir, args.kind)
    print(f"Building {info['version']} ({info['fingerprint'][:12]})", file=sys.stderr)
    env = build_env(info)
    # A plain `idf.py build` skips cmake when no CMakeLists changed, which would
    # silently bake in whatever version was configured last. Reconfigure first so
    # WQN_RELEASE_VERSION always reaches the compile definitions.
    run(["idf.py", "--no-ccache", "-B", args.build_dir, "reconfigure"], env=env)
    run(["idf.py", "--no-ccache", "-B", args.build_dir, "build"], env=env)
    run_architecture_gate()
    write_record(info, args.build_dir)
    return 0


# --------------------------------------------------------------------------
# record-build / record-flash
# --------------------------------------------------------------------------


def previous_release_dir(version: str) -> Path | None:
    versions = V.ordered_versions(V.load_version_index())
    if version not in versions:
        return None
    index = versions.index(version)
    return release_dir(versions[index - 1]) if index > 0 else None


def load_json(path: Path) -> dict | None:
    if not path.is_file():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


def render_updatelog(info: dict, manifest: dict[str, str], prev_dir: Path | None) -> str:
    stamp = datetime.now().astimezone().strftime("%Y-%m-%d %H:%M %Z")
    modified, untracked = V.changed_file_count()
    lines = [
        f"## {info['version']} — {stamp} ({'dirty' if info['dirty'] else 'clean'})",
        "",
        f"Base commit: {info['base_commit_short']} "
        f"({modified} modified, {untracked} untracked)",
        f"Fingerprint: {info['fingerprint']}",
        f"Profile: {info['profile']} / IDF {info['idf']}",
        "",
    ]

    if prev_dir is None:
        lines.append("Changed vs: (first recorded version)")
    else:
        prev_manifest = load_json(prev_dir / "source-manifest.json")
        lines.append(f"Changed vs {prev_dir.name}:")
        if prev_manifest is None:
            lines.append("  (previous source manifest unavailable)")
        else:
            changes = V.diff_manifests(prev_manifest, manifest)
            if not changes:
                lines.append("  (no source change)")
            for status, path in changes:
                lines.append(f"  {status} {path}")

    prev_identity = load_json(prev_dir / "identity.json") if prev_dir else None
    commits = V.recent_commits((prev_identity or {}).get("base_commit", ""))
    if commits:
        lines.append("")
        lines.append("Recent commits:")
        lines.extend(f"  {line}" for line in commits)

    lines.append("")
    return "\n".join(lines)


def write_record(info: dict, build_dir: str | None) -> Path:
    target = release_dir(info["version"])
    target.mkdir(parents=True, exist_ok=True)

    manifest = V.source_manifest()
    # Editing sources while a build is running would otherwise give the artifact
    # one version and the record another.
    actual = V.fingerprint(manifest, info["profile"], info["idf"])
    if actual != info["fingerprint"]:
        raise SystemExit(
            f"source changed during the build ({info['fingerprint'][:12]} -> "
            f"{actual[:12]}); re-run to get a consistent version"
        )

    (target / "source-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    V.write_changes_patch(target / "changes.patch")
    sdkconfig = V.PROJECT_DIR / "sdkconfig"
    if sdkconfig.is_file():
        shutil.copy2(sdkconfig, target / "sdkconfig")

    app = app_artifact(build_dir)
    identity = {
        **info,
        "created_at": V.utc_now_iso(),
        "machine": socket.gethostname(),
        "app": app,
    }
    (target / "identity.json").write_text(
        json.dumps(identity, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    (target / "UPDATELOG.md").write_text(
        render_updatelog(info, manifest, previous_release_dir(info["version"])),
        encoding="utf-8",
    )

    print(f"Recorded {info['version']} -> {target}", file=sys.stderr)
    return target


def cmd_record_build(args: argparse.Namespace) -> int:
    write_record(collect_identity(args.build_dir, args.kind), args.build_dir)
    return 0


def app_artifact(build_dir: str | None) -> dict | None:
    flasher_args = resolve_build_dir(build_dir) / "flasher_args.json"
    data = load_json(flasher_args)
    if not data:
        return None
    app = data.get("app") or {}
    name = Path(app.get("file", "")).name
    if not name:
        return None
    path = resolve_build_dir(build_dir) / app.get("file", "")
    return {
        "offset": app.get("offset"),
        "file": name,
        "size": path.stat().st_size if path.is_file() else None,
        "sha256": V.sha256_file(path) if path.is_file() else None,
    }


def cmd_record_flash(args: argparse.Namespace) -> int:
    info = collect_identity(args.build_dir, "dev")
    entry = {
        "ts": V.utc_now_iso(),
        "machine": args.machine or socket.gethostname(),
        "port": args.port,
        "status": args.status,
        "version": info["version"],
        "fingerprint": info["fingerprint"],
        "base_commit": info["base_commit_short"],
        "dirty": info["dirty"],
        "app_sha256": (app_artifact(args.build_dir) or {}).get("sha256"),
        "record_dir": f"dist/{info['version']}",
    }
    history = dist_dir() / HISTORY_NAME
    history.parent.mkdir(parents=True, exist_ok=True)
    with history.open("a", encoding="utf-8") as f:
        f.write(json.dumps(entry, ensure_ascii=False) + "\n")
    print(f"Flash recorded: {info['version']} on {args.port} ({args.status})", file=sys.stderr)
    return 0


# --------------------------------------------------------------------------
# package / publish
# --------------------------------------------------------------------------

PORTABLE_FLASHER = V.PROJECT_DIR / "tools" / "portable-flasher"
ESPTOOL_EXE = PORTABLE_FLASHER / ".cache" / "esptool.exe"
DEFAULT_REMOTE_BASE = "/www/wwwroot/alist_storage/WQN NOTE 4"


def remote_join(base: str, *parts: str) -> str:
    return "/".join([base.rstrip("/"), *parts])


def ssh_capture(host: str, command: str) -> str:
    result = subprocess.run(
        ["ssh", host, command],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    return result.stdout if result.returncode == 0 else ""


def scp_to(host: str, local: Path, remote: str) -> None:
    run(["scp", "-q", str(local), f"{host}:{remote}"])


def load_remote_index(host: str, base: str) -> dict:
    raw = ssh_capture(host, f"cat {shlex.quote(remote_join(base, 'index.json'))}")
    if not raw.strip():
        return {"schema": 1, "project": V.PROJECT_DIR.name, "latest": None, "days": {}}
    return json.loads(raw)


def store_remote_file(host: str, base: str, name: str, text: str) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        local = Path(tmp) / name
        local.write_text(text, encoding="utf-8")
        scp_to(host, local, remote_join(base, name))


def latest_release(index: dict) -> str | None:
    releases = [
        version
        for version in V.ordered_versions(index)
        if (V.record_from(index, version) or {}).get("kind") == "release"
    ]
    return releases[-1] if releases else None


def find_zip(target: Path) -> Path:
    zips = sorted(target.glob("WQN-Note4-Flasher-*.zip"))
    if not zips:
        raise SystemExit(f"No flasher zip found in {target}")
    return zips[-1]


def ensure_esptool() -> None:
    run(
        [
            sys.executable,
            str(PORTABLE_FLASHER / "ensure_portable_esptool.py"),
            "--output",
            str(ESPTOOL_EXE),
        ]
    )


def cmd_package(args: argparse.Namespace) -> int:
    info = collect_identity(args.build_dir, "release")
    target = write_record(info, args.build_dir)
    V.write_source_snapshot(target / "source-snapshot.tgz")
    ensure_esptool()

    command = [
        sys.executable,
        str(PORTABLE_FLASHER / "package_flasher.py"),
        "--build-dir",
        args.build_dir,
        "--output-dir",
        str(target),
        "--esptool-exe",
        str(ESPTOOL_EXE),
        "--release-version",
        info["version"],
        "--extra",
        f"{target / 'UPDATELOG.md'}::UPDATELOG.md",
    ]
    run(command)

    zip_path = find_zip(target)
    print(f"Packaged {info['version']} -> {zip_path}", file=sys.stderr)
    return 0


def cmd_publish(args: argparse.Namespace) -> int:
    info = collect_identity(args.build_dir, "release")
    target = release_dir(info["version"])
    identity = load_json(target / "identity.json")
    if identity is None:
        raise SystemExit(f"No local record for {info['version']}; run `package` first")

    zip_path = find_zip(target)
    zip_sha = V.sha256_file(zip_path)
    remote_ver = remote_join(args.remote_base, "v", info["version"])

    print(f"Publishing {info['version']} -> {args.ssh_host}:{remote_ver}", file=sys.stderr)
    run(["ssh", args.ssh_host, f"mkdir -p {shlex.quote(remote_ver)}"])
    for path in sorted(target.iterdir()):
        if path.is_file():
            scp_to(args.ssh_host, path, f"{remote_ver}/{path.name}")

    remote_sha = ssh_capture(
        args.ssh_host, f"sha256sum {shlex.quote(f'{remote_ver}/{zip_path.name}')}"
    ).split()
    if not remote_sha or remote_sha[0] != zip_sha:
        raise SystemExit(f"Remote sha256 mismatch for {zip_path.name}")

    index = load_remote_index(args.ssh_host, args.remote_base)
    date, _, serial = info["version"].rpartition(".")
    day = index.setdefault("days", {}).setdefault(date, {})
    existing = day.get(serial)
    if existing and existing.get("fingerprint") != info["fingerprint"]:
        raise SystemExit(
            f"Version {info['version']} is already published with a different fingerprint; "
            f"re-run to allocate a fresh serial"
        )
    day[serial] = {
        "version": info["version"],
        "kind": "release",
        "fingerprint": info["fingerprint"],
        "base_commit": info["base_commit"],
        "dirty": info["dirty"],
        "profile": info["profile"],
        "idf": info["idf"],
        "chip": (load_json(resolve_build_dir(args.build_dir) / "flasher_args.json") or {})
        .get("extra_esptool_args", {})
        .get("chip", "esp32s3"),
        "app": app_artifact(args.build_dir),
        "zip": f"v/{info['version']}/{zip_path.name}",
        "zip_sha256": zip_sha,
        "created_at": identity.get("created_at", V.utc_now_iso()),
        "machine": identity.get("machine"),
    }
    index["latest"] = latest_release(index)
    store_remote_file(
        args.ssh_host, args.remote_base, "index.json", json.dumps(index, indent=2, ensure_ascii=False)
    )
    store_remote_file(
        args.ssh_host,
        args.remote_base,
        "latest.json",
        json.dumps(
            {
                "version": info["version"],
                "kind": "release",
                "fingerprint": info["fingerprint"],
                "dirty": info["dirty"],
                "app": day[serial]["app"],
                "zip": day[serial]["zip"],
                "zip_sha256": zip_sha,
                "published_at": V.utc_now_iso(),
            },
            indent=2,
            ensure_ascii=False,
        ),
    )

    section = (target / "UPDATELOG.md").read_text(encoding="utf-8")
    remote_log = ssh_capture(
        args.ssh_host, f"cat {shlex.quote(remote_join(args.remote_base, 'UPDATELOG.md'))}"
    )
    if f"## {info['version']} " not in remote_log:
        joined = section if not remote_log.strip() else f"{section}\n{remote_log}"
        store_remote_file(args.ssh_host, args.remote_base, "UPDATELOG.md", joined)

    print(f"Published {info['version']}; remote latest = {index['latest']}", file=sys.stderr)
    return 0


def cmd_release(args: argparse.Namespace) -> int:
    """One-click: build, record, package and publish the current working tree."""
    args.kind = "release"
    cmd_build(args)
    cmd_package(args)
    return cmd_publish(args)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="WQN Note4 firmware release tooling")
    sub = parser.add_subparsers(dest="command", required=True)

    identity = sub.add_parser("identity", help="resolve the version for this working tree")
    identity.add_argument("--build-dir", default=V.DEFAULT_BUILD_DIR)
    identity.add_argument("--kind", choices=("dev", "release"), default="dev")
    identity.add_argument("--format", choices=("export", "json", "text"), default="text")
    identity.set_defaults(func=cmd_identity)

    build = sub.add_parser("build", help="build the firmware and record its diff")
    build.add_argument("--build-dir", default=V.DEFAULT_BUILD_DIR)
    build.add_argument("--kind", choices=("dev", "release"), default="dev")
    build.set_defaults(func=cmd_build)

    record_build = sub.add_parser("record-build", help="persist the diff for this version")
    record_build.add_argument("--build-dir", default=V.DEFAULT_BUILD_DIR)
    record_build.add_argument("--kind", choices=("dev", "release"), default="dev")
    record_build.set_defaults(func=cmd_record_build)

    record_flash = sub.add_parser("record-flash", help="append a flash event to the history")
    record_flash.add_argument("--build-dir", default=V.DEFAULT_BUILD_DIR)
    record_flash.add_argument("--port", required=True)
    record_flash.add_argument("--status", default="ok")
    record_flash.add_argument("--machine", default=None)
    record_flash.set_defaults(func=cmd_record_flash)

    package = sub.add_parser("package", help="build the portable flasher zip")
    package.add_argument("--build-dir", default=V.DEFAULT_BUILD_DIR)
    package.set_defaults(func=cmd_package)

    publish = sub.add_parser("publish", help="upload a packaged version and update the index")
    publish.add_argument("--build-dir", default=V.DEFAULT_BUILD_DIR)
    publish.add_argument("--ssh-host", default="aliyun")
    publish.add_argument("--remote-base", default=DEFAULT_REMOTE_BASE)
    publish.set_defaults(func=cmd_publish)

    release = sub.add_parser("release", help="package and publish")
    release.add_argument("--build-dir", default=V.DEFAULT_BUILD_DIR)
    release.add_argument("--ssh-host", default="aliyun")
    release.add_argument("--remote-base", default=DEFAULT_REMOTE_BASE)
    release.set_defaults(func=cmd_release)

    return parser


def main() -> int:
    args = build_parser().parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
