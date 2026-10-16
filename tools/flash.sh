#!/bin/bash
# Flash a yotwo release to the player over the serial cable.
#
# Writes the four regions the two-slot layout needs: bootloader, partition
# table, blank otadata (so the bootloader starts ota_0) and the app into ota_0.
# Uses firmware/releases/ if the release is there (after tools/release.sh),
# otherwise downloads it from the GitHub release.
#
# Usage: tools/flash.sh [--version <ver>] [--port <dev>] [--first] [--list]
#   --version  default: latest (the newest local build, else the newest GitHub release)
#   --port     default: the only USB-serial adapter present
#   --first    first install over the original firmware: also erases the old
#              settings area (0x9000-0xFFFF) so yotwo starts clean
#   --list     list local and published versions
#
# The player has no auto-reset: put it in bootloader mode first (docs/getting-started.md).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
rel="$root/firmware/releases"
repo="tomekent/yotwo"
version=latest
port=""
first=0

while [ $# -gt 0 ]; do
  case "$1" in
    --version|-v) version="${2:?--version needs a value}"; shift 2;;
    --port|-p)    port="${2:?--port needs a value}"; shift 2;;
    --first)      first=1; shift;;
    --list|-l)
      echo "local:"; ls "$rel" 2>/dev/null | sed -n 's/^yotwo-\(.*\)\.bin$/  \1/p'
      echo "published:"; curl -fsSL "https://api.github.com/repos/$repo/releases" 2>/dev/null \
        | python3 -c 'import json,sys; [print("  " + r["tag_name"][1:]) for r in json.load(sys.stdin)]' || echo "  (could not reach GitHub)"
      exit 0;;
    -h|--help)    sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    *) echo "unknown option: $1 (try --help)" >&2; exit 2;;
  esac
done
version="${version#v}"

# esptool: through uv if present (nothing installed), else an installed copy.
if command -v uvx >/dev/null; then esptool=(uvx --from esptool esptool.py)
elif command -v esptool.py >/dev/null; then esptool=(esptool.py)
elif command -v esptool >/dev/null; then esptool=(esptool)
else echo "esptool not found: install uv (https://docs.astral.sh/uv/) or 'pip install esptool'" >&2; exit 1
fi

app="$rel/yotwo-$version.bin"
boot="$rel/boot"
if [ ! -f "$app" ] || [ ! -f "$boot/bootloader.bin" ]; then
  # Not built here: fetch the release's cable bundle from GitHub.
  tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
  if [ "$version" = latest ]; then api="https://api.github.com/repos/$repo/releases/latest"
  else api="https://api.github.com/repos/$repo/releases/tags/v$version"; fi
  url=$(curl -fsSL "$api" | python3 -c '
import json, sys
for a in json.load(sys.stdin)["assets"]:
    if a["name"].endswith("-cable.zip"):
        print(a["browser_download_url"]); break') || { echo "no such release on GitHub: $version (try --list)" >&2; exit 1; }
  [ -n "$url" ] || { echo "release $version has no cable bundle" >&2; exit 1; }
  echo "downloading $(basename "$url")"
  curl -fsSL -o "$tmp/cable.zip" "$url"
  (cd "$tmp" && unzip -q cable.zip)
  dir="$(dirname "$(find "$tmp" -name bootloader.bin | head -1)")"
  app="$(find "$dir" -name 'yotwo-*.bin' | head -1)"
  boot="$dir"
fi
for f in bootloader.bin partition-table.bin ota_data_initial.bin; do
  [ -f "$boot/$f" ] || { echo "missing $boot/$f" >&2; exit 1; }
done

if [ -z "$port" ]; then
  shopt -s nullglob
  ports=(/dev/cu.usbserial-* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART* /dev/cu.usbmodem* /dev/ttyUSB* /dev/ttyACM*)
  shopt -u nullglob
  if [ ${#ports[@]} -eq 0 ]; then echo "no USB-serial adapter found: is it plugged in? (or give --port)" >&2; exit 1; fi
  if [ ${#ports[@]} -gt 1 ]; then echo "several ports (${ports[*]}): pick one with --port" >&2; exit 1; fi
  port="${ports[0]}"
fi
if command -v lsof >/dev/null && holders=$(lsof -t "$port" 2>/dev/null); then
  echo "$port is in use by pid(s) $holders: close the serial monitor first" >&2; exit 1
fi

inside=$(python3 -c '
import sys, struct
d = open(sys.argv[1], "rb").read(80)
if struct.unpack("<I", d[32:36])[0] != 0xABCD5432:
    sys.exit("not an ESP-IDF app image: " + sys.argv[1])
print(d[48:80].split(b"\0")[0].decode())' "$app")
echo "flashing yotwo $inside ($(basename "$app")) to $port"

# 115200 baud, no reset before or after: the player's serial link is only
# reliable at this speed, and its reset lines are not wired to the adapter.
common=(--chip esp32 --port "$port" --baud 115200 --before no_reset --after no_reset)
if [ "$first" = 1 ]; then
  echo "erasing the old settings area (0x9000-0xFFFF)"
  "${esptool[@]}" "${common[@]}" erase_region 0x9000 0x7000
fi
"${esptool[@]}" "${common[@]}" write_flash \
  --flash_mode dio --flash_freq 40m --flash_size 8MB \
  0x1000 "$boot/bootloader.bin" \
  0x8000 "$boot/partition-table.bin" \
  0x790000 "$boot/ota_data_initial.bin" \
  0x10000 "$app"
echo "done: reset the player (or power it off and on) to start yotwo"
