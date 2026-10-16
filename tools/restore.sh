#!/bin/bash
# Put the original firmware back: write a full backup made by tools/backup.sh
# over the whole flash. This undoes yotwo completely (its settings go too).
#
# Usage: tools/restore.sh [--port <dev>] <backup.bin>
#
# Takes about 13 minutes at 115200 baud. The player must be in bootloader
# mode. esptool checks what it wrote against the file before finishing.
set -euo pipefail
port=""
file=""
while [ $# -gt 0 ]; do
  case "$1" in
    --port|-p) port="${2:?--port needs a value}"; shift 2;;
    -h|--help) sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    *) file="$1"; shift;;
  esac
done
[ -f "$file" ] || { echo "usage: tools/restore.sh [--port <dev>] <backup.bin>" >&2; exit 2; }
size=$(wc -c < "$file" | tr -d ' ')
[ "$size" = 8388608 ] || { echo "$file is $size bytes, not a full 8 MB backup" >&2; exit 1; }

if command -v uvx >/dev/null; then esptool=(uvx --from esptool esptool.py)
elif command -v esptool.py >/dev/null; then esptool=(esptool.py)
elif command -v esptool >/dev/null; then esptool=(esptool)
else echo "esptool not found: install uv (https://docs.astral.sh/uv/) or 'pip install esptool'" >&2; exit 1
fi
if [ -z "$port" ]; then
  shopt -s nullglob
  ports=(/dev/cu.usbserial-* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART* /dev/cu.usbmodem* /dev/ttyUSB* /dev/ttyACM*)
  shopt -u nullglob
  if [ ${#ports[@]} -ne 1 ]; then echo "found ${#ports[@]} serial ports: pick one with --port" >&2; exit 1; fi
  port="${ports[0]}"
fi
echo "restoring $file to $port"
"${esptool[@]}" --chip esp32 --port "$port" --baud 115200 --before no_reset --after no_reset \
  write_flash --flash_mode keep --flash_freq keep --flash_size keep 0x0 "$file"
echo "done: reset the player to start the original firmware"
