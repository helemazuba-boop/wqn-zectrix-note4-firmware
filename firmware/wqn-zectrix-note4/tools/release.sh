#!/usr/bin/env bash
# One-click firmware release: build, record the source diff, package the
# portable Windows flasher and publish it.
#
#   ./tools/release.sh                 # build + package + publish
#   ./tools/release.sh --no-upload     # not supported here; use `release.py package`
#
# ESP-IDF lives in WSL, so the toolchain is loaded here rather than in Python.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

IDF_EXPORT="${WQN_IDF_EXPORT:-/home/unknow/esp/esp-idf-v5.5/export.sh}"
if [ -f "$IDF_EXPORT" ]; then
    # shellcheck disable=SC1090
    source "$IDF_EXPORT" >/dev/null 2>&1
else
    echo "WARNING: ESP-IDF export script not found: $IDF_EXPORT" >&2
    echo "         Set WQN_IDF_EXPORT to override." >&2
fi

exec python3 tools/release/release.py "$@"
