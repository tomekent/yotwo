#!/bin/bash
# Make firmware/releases/yotwo-<version>-cable.zip from an archived release:
# the app, the three boot files and FLASHING.txt, everything needed to put that
# version on a player by cable without cloning the repo. Prints the zip's path.
#
# Usage: tools/cable_zip.sh <version>
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
rel="$root/firmware/releases"
v="${1:?usage: tools/cable_zip.sh <version>}"
v="${v#v}"
app="$rel/yotwo-$v.bin"
[ -f "$app" ] || { echo "no $app" >&2; exit 1; }

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
d="$tmp/yotwo-$v"
mkdir "$d"
cp "$app" "$rel/boot/bootloader.bin" "$rel/boot/partition-table.bin" "$rel/boot/ota_data_initial.bin" "$d/"
cat > "$d/FLASHING.txt" <<EOF
yotwo $v: flashing by cable
===========================

Full guide: https://github.com/tomekent/yotwo/blob/main/docs/getting-started.md

1. Install esptool:  pip install esptool   (or use uv: uvx --from esptool esptool.py ...)
2. Put the player in bootloader mode (hold BOOT/GPIO0 low, tap RESET/EN, let go).
3. From this folder, with your serial port in place of PORT
   (macOS /dev/cu.usbserial-XXXX, Linux /dev/ttyUSB0, Windows COM3):

   First install only (clears the original firmware's settings area):
     esptool.py --chip esp32 --port PORT --baud 115200 --before no_reset --after no_reset erase_region 0x9000 0x7000

   Then (first install and later cable updates; put the player back into
   bootloader mode first if you just ran the erase and it complains):
     esptool.py --chip esp32 --port PORT --baud 115200 --before no_reset --after no_reset write_flash --flash_mode dio --flash_freq 40m --flash_size 8MB 0x1000 bootloader.bin 0x8000 partition-table.bin 0x790000 ota_data_initial.bin 0x10000 yotwo-$v.bin

4. Reset the player. It shows a Wi-Fi setup screen; see the guide.

Once yotwo is on the player, later versions can be installed over Wi-Fi from
its web page (Settings > Firmware, upload yotwo-<version>.bin).
EOF
out="$rel/yotwo-$v-cable.zip"
rm -f "$out"
(cd "$tmp" && zip -q -r -X "$out" "yotwo-$v")
echo "$out"
