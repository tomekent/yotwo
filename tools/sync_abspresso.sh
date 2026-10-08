#!/bin/sh
# Copy the shared (hardware-independent) parts of ABSpresso into the Yoto firmware, apply
# this project's local changes on top, and record which commit the copies came from.
#
# Usage: tools/sync_abspresso.sh [path-to-ABSpresso]
#
# ABSpresso itself is left untouched: changes this project needs in the shared code are
# kept in firmware/main/abspresso/local.patch and re-applied here. To change the shared
# code, edit the files under firmware/main/abspresso/ and run tools/sync_abspresso.sh
# --update-patch to regenerate local.patch from the difference.
set -e
cd "$(dirname "$0")/.."
UPDATE=0
if [ "$1" = "--update-patch" ]; then UPDATE=1; shift; fi
SRC="${1:-$HOME/ESP32/Esp32-1.85-touch}"
DST=firmware/main/abspresso
FILES="
network/wifi.c network/wifi.h
network/portal.c network/portal.h network/portal_page.h
network/abs_api.c network/abs_api.h
app/player.c app/player.h
app/download.c app/download.h
app/catalog.c app/catalog.h
app/config.c app/config.h
app/text.c app/text.h
"
if [ "$UPDATE" = 1 ]; then
  TMP=$(mktemp -d)
  mkdir -p "$TMP/a/network" "$TMP/a/app" "$TMP/b/network" "$TMP/b/app"
  for f in $FILES; do cp "$SRC/main/$f" "$TMP/a/$f"; cp "$DST/$f" "$TMP/b/$f"; done
  (cd "$TMP" && diff -u -r a b | sed -E 's/^(---|\+\+\+) ([ab]\/[^[:space:]]*).*/\1 \2/' | grep -v '^diff -u' || true) > "$DST/local.patch"
  rm -rf "$TMP"
  echo "local.patch: $(grep -c '^@@' "$DST/local.patch") hunk(s) in $(grep -c '^+++ ' "$DST/local.patch") file(s)"
  exit 0
fi
mkdir -p "$DST/network" "$DST/app"
for f in $FILES; do cp "$SRC/main/$f" "$DST/$f"; done
if [ -s "$DST/local.patch" ]; then
  patch -p1 -d "$DST" --no-backup-if-mismatch < "$DST/local.patch"
fi
{
  echo "Source: $(git -C "$SRC" remote get-url origin)"
  echo "Commit: $(git -C "$SRC" rev-parse HEAD)"
  echo "Dirty:  $(git -C "$SRC" status --porcelain -- main | wc -l | tr -d ' ') modified file(s) under main/"
  echo "Local:  $(grep -c '^+++ ' "$DST/local.patch" 2>/dev/null || echo 0) file(s) changed by local.patch"
  echo "Synced: $(date +%Y-%m-%d)"
} > "$DST/SOURCE.txt"
cat "$DST/SOURCE.txt"
