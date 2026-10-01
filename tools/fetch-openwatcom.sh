#!/bin/sh
# Download the pinned OpenWatcom build that reproduces ROGUE.EXE, check its SHA-256, unpack it.
#   tools/fetch-openwatcom.sh [DIR]      then: export WATCOM=DIR
# 2026-10-01-Build has the same wcc, wlink, wdis, wlib and clibl.lib as the toolchain the
# Rogue build was first reproduced with. Change both lines together, and check that
# `make test-rogue-build` still reproduces the same bytes.
set -eu
URL=https://github.com/open-watcom/open-watcom-v2/releases/download/2026-10-01-Build/ow-snapshot.tar.xz
SHA256=e6aa1b1e40ac8bbf97658d2c70fff8a4242d6ca4a1c60806f2baa5317083d4fe
dest=${1:-build/openwatcom}
if [ -x "$dest/binl64/wcc" ]; then exit 0; fi
mkdir -p "$dest"
archive="$dest.tar.xz"
curl -fsSL -o "$archive" "$URL"
echo "$SHA256  $archive" | sha256sum -c - >/dev/null || { echo "OpenWatcom archive checksum mismatch: $URL" >&2; exit 1; }
tar xf "$archive" -C "$dest"
rm -f "$archive"
