#!/usr/bin/env python3
"""Pick which firmware worktree deploy.bat should build and flash.

deploy.bat runs this with no arguments to render the menu, then again with
--resolve to turn the picked numbers into paths.

Ordering: newest activity first, where activity is
    max(HEAD committer time, mtime of the newest changed/untracked file)
so a clean tree sorts by its last commit and a tree you are editing right
now sorts to the top even when its HEAD is old.

Only worktrees that actually contain this firmware project are listed.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from datetime import datetime
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parents[1]


def git(root: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(root), *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    return result.stdout if result.returncode == 0 else ""


def repo_root() -> Path:
    out = git(PROJECT_DIR, "rev-parse", "--show-toplevel").strip()
    if not out:
        raise SystemExit("not a git repository: %s" % PROJECT_DIR)
    return Path(out)


REPO_ROOT = repo_root()
REL = PROJECT_DIR.relative_to(REPO_ROOT)


def worktree_roots() -> list[Path]:
    roots: list[Path] = []
    for block in git(REPO_ROOT, "worktree", "list", "--porcelain").split("\n\n"):
        for line in block.splitlines():
            if line.startswith("worktree "):
                roots.append(Path(line[len("worktree "):].strip()))
    return roots


def newest_activity(root: Path) -> tuple[float, bool]:
    """(activity timestamp, is dirty) for the firmware project in this tree."""
    stamp = 0.0
    raw = git(root, "log", "-1", "--format=%ct").strip()
    if raw:
        stamp = float(raw)
    status = git(root, "status", "--porcelain", "-z", "--", REL.as_posix())
    dirty = bool(status.strip("\0").strip())
    for entry in status.split("\0"):
        if len(entry) < 4:
            continue
        try:
            stamp = max(stamp, (root / entry[3:]).stat().st_mtime)
        except OSError:
            pass
    return stamp, dirty


def label(root: Path) -> str:
    branch = git(root, "rev-parse", "--abbrev-ref", "HEAD").strip()
    if branch and branch != "HEAD":
        return branch
    sha = git(root, "rev-parse", "--short=8", "HEAD").strip() or "?"
    return "detached@%s" % sha


def shorten(root: Path) -> str:
    home = str(Path.home())
    text = str(root)
    if text == home or text.startswith(home + "/"):
        return "~" + text[len(home):]
    return text


def collect() -> list[dict]:
    rows = []
    for root in worktree_roots():
        fw = root / REL
        if not (fw / "CMakeLists.txt").is_file():
            continue
        stamp, dirty = newest_activity(root)
        rows.append(
            {
                "root": root,
                "fw": fw,
                "stamp": stamp,
                "dirty": dirty,
                "label": label(root),
            }
        )
    rows.sort(key=lambda row: (-row["stamp"], str(row["root"])))
    return rows


def render(rows: list[dict]) -> None:
    if not rows:
        raise SystemExit("no worktree contains %s" % REL)
    width = min(max(len(row["label"]) for row in rows), 34)
    print("  #  modified          state  %-*s  tree" % (width, "branch"))
    for index, row in enumerate(rows, start=1):
        when = datetime.fromtimestamp(row["stamp"]).strftime("%m-%d %H:%M")
        mark = "*" if row["root"].resolve() == PROJECT_DIR.resolve() else " "
        print(
            "%3d%s %s  %-5s  %-*s  %s"
            % (
                index,
                mark,
                when,
                "dirty" if row["dirty"] else "clean",
                width,
                row["label"],
                shorten(row["root"]),
            )
        )
    print("       (* = this checkout)")


def resolve(rows: list[dict], picks: str) -> int:
    chosen: list[int] = []
    for part in picks.replace(" ", ",").replace(chr(13), ",").split(","):
        part = part.strip()
        if not part:
            continue
        if not part.isdigit():
            print("invalid selection: %s" % part, file=sys.stderr)
            return 2
        number = int(part)
        if not 1 <= number <= len(rows):
            print("selection out of range: %d (1-%d)" % (number, len(rows)), file=sys.stderr)
            return 2
        if number not in chosen:
            chosen.append(number)
    if not chosen:
        print("nothing selected", file=sys.stderr)
        return 2
    for number in chosen:
        print(rows[number - 1]["fw"])
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resolve", metavar="N[,M...]", help="print the picked trees' firmware paths")
    args = parser.parse_args()
    rows = collect()
    if args.resolve:
        return resolve(rows, args.resolve)
    render(rows)
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())
