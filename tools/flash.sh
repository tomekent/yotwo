#!/bin/bash
# Flash an archived release to the player over the serial cable.
#
# Writes the four regions the two-slot layout needs: bootloader, partition
# table, blank otadata (so the bootloader starts ota_0) and the app into ota_0.
# All from firmware/releases/, so it doesn't depend on the build folder.
#
# Usage: tools/flash.sh [--version <ver>] [--port <dev>] [--list]
#   default version: latest (releases/yotwo-latest.bin)
#   default port:    the only /dev/cu.usbserial-* present
#
# The link has no auto-reset: put the player in bootloader mode first.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
rel="$root/firmware/releases"
version=latest
port=""

while [ $# -gt 0 ]; do
  case "$1" in
    --version|-v) version="${2:?--version needs a value}"; shift 2;;
    --port|-p)    port="${2:?--port needs a value}"; shift 2;;
    --list|-l)    ls "$rel" | sed -n 's/^yotwo-\(.*\)\.bin$/\1/p'; exit 0;;
    -h|--help)    sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    *) echo "unknown option: $1 (try --help)" >&2; exit 2;;
  esac
done

app="$rel/yotwo-${version#v}.bin"
[ -f "$app" ] || { echo "no $app (try --list)" >&2; exit 1; }
for f in bootloader.bin partition-table.bin ota_data_initial.bin; do
  [ -f "$rel/boot/$f" ] || { echo "missing $rel/boot/$f: run tools/release.sh" >&2; exit 1; }
done

if [ -z "$port" ]; then
  ports=(/dev/cu.usbserial-*)
  if [ ! -e "${ports[0]}" ]; then echo "no /dev/cu.usbserial-* found: is the adapter plugged in?" >&2; exit 1; fi
  if [ ${#ports[@]} -gt 1 ]; then echo "several ports (${ports[*]}): pick one with --port" >&2; exit 1; fi
  port="${ports[0]}"
fi
if holders=$(lsof -t "$port" 2>/dev/null); then
  echo "$port is in use by pid(s) $holders: close the serial monitor first" >&2; exit 1
fi

inside=$(python3 -c '
import sys, struct
d = open(sys.argv[1], "rb").read(80)
if struct.unpack("<I", d[32:36])[0] != 0xABCD5432:
    sys.exit("not an ESP-IDF app image: " + sys.argv[1])
print(d[48:80].split(b"\0")[0].decode())' "$app")
echo "flashing yotwo $inside ($(basename "$app")) to $port"

exec uvx --from esptool esptool.py --chip esp32 --port "$port" --baud 115200 \
  --before no_reset --after no_reset write_flash \
  --flash_mode dio --flash_freq 40m --flash_size 8MB \
  0x1000 "$rel/boot/bootloader.bin" \
  0x8000 "$rel/boot/partition-table.bin" \
  0x790000 "$rel/boot/ota_data_initial.bin" \
  0x10000 "$app"
