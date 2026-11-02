#!/bin/sh
# Tests, builds and packages Floater. CI runs this inside the SteamOS holo-base image so the
# helper links against the same libraries as SteamOS.
set -eu
cd "$(dirname "$0")/.."

pnpm install --frozen-lockfile
python3 -m unittest discover -s tests -t . -v
# LeakSanitizer needs ptrace, which CI containers usually don't allow.
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" make -C backend WERROR=-Werror test
pnpm run build
make -C backend WERROR=-Werror
python3 scripts/package.py
