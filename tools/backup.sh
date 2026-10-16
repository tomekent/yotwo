#!/bin/bash
# Back up the player's whole 8 MB flash before installing yotwo, so the
# original firmware can be put back later (tools/restore.sh).
#
# Usage: tools/backup.sh [--port <dev>] [output.bin]   (default: yotwo-backup-<MAC>.bin)
#
# Reads the flash in 512 KB pieces. Each piece is checked by esptool against
# an MD5 the chip computes, so a piece is either right or rejected. The
# serial link on these players drops bytes now and then; when a piece fails,
# put the player back into bootloader mode and run this again: it carries on
# from the first missing piece. At 115200 baud the whole read takes about
# 13 minutes.
#
# KEEP THE BACKUP PRIVATE. It holds your Wi-Fi password and the player's own
# device certificate and keys.
set -euo pipefail
port=""
out=""
while [ $# -gt 0 ]; do
  case "$1" in
    --port|-p) port="${2:?--port needs a value}"; shift 2;;
    -h|--help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    *) out="$1"; shift;;
  esac
done

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
common=(--chip esp32 --port "$port" --baud 115200 --before no_reset --after no_reset)

if [ -z "$out" ]; then
  mac=$("${esptool[@]}" "${common[@]}" read_mac 2>/dev/null | sed -n 's/^MAC: //p' | head -1 | tr -d ':')
  [ -n "$mac" ] || { echo "no answer from the player: is it in bootloader mode?" >&2; exit 1; }
  out="yotwo-backup-$mac.bin"
fi
parts="$out.parts"
mkdir -p "$parts"

for i in $(seq 0 15); do
  f="$parts/p$(printf %02d "$i").bin"
  if [ -f "$f" ] && [ "$(wc -c < "$f" | tr -d ' ')" = 524288 ]; then continue; fi
  off=$(printf "0x%x" $((i * 0x80000)))
  echo "reading piece $((i + 1)) of 16 ($off)"
  if "${esptool[@]}" "${common[@]}" read_flash "$off" 0x80000 "$f.tmp" > "$parts/log.txt" 2>&1; then
    mv "$f.tmp" "$f"
  else
    rm -f "$f.tmp"
    echo "piece $((i + 1)) failed: $(grep -m1 -E 'rror|Corrupt|Failed' "$parts/log.txt" || echo 'no reply')" >&2
    echo "put the player back into bootloader mode and run this again to carry on" >&2
    exit 1
  fi
done
cat "$parts"/p*.bin > "$out"
rm -rf "$parts"
echo "backup complete: $out ($(wc -c < "$out" | tr -d ' ') bytes)"
if command -v shasum >/dev/null; then shasum -a 256 "$out"; else sha256sum "$out"; fi
echo "keep it somewhere private: it holds your Wi-Fi password and the player's keys"
