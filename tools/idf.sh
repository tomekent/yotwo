#!/bin/bash
# Run idf.py for the yotwo firmware with the ESP-IDF v5.5.2 environment loaded.
#
# Usage: tools/idf.sh build | size | menuconfig | monitor -p <port> | ...
# ESP-IDF is looked for in $IDF_PATH, else ~/esp/v5.5.2/esp-idf.
#
# Flashing is not wrapped here on purpose: the player has no auto-reset, so
# use tools/flash.sh (after tools/release.sh) with the player in bootloader mode.
set -e
cd "$(dirname "$0")/../firmware"
# On the author's Mac the IDF Python environment was made with Homebrew's Python 3.14,
# which then has to come first on PATH. Harmless where that path does not exist.
[ -d /opt/homebrew/opt/python@3.14/libexec/bin ] && \
  export PATH="/opt/homebrew/opt/python@3.14/libexec/bin:/opt/homebrew/opt/python@3.14/bin:$PATH"
idf="${IDF_PATH:-$HOME/esp/v5.5.2/esp-idf}"
[ -f "$idf/export.sh" ] || { echo "ESP-IDF not found at $idf: set IDF_PATH (see docs/building.md)" >&2; exit 1; }
. "$idf/export.sh" >/dev/null
case "$1" in
  flash|erase-flash|erase_flash|app-flash|bootloader-flash|encrypted-flash|efuse-*)
    echo "refusing '$1': use tools/flash.sh with the player in bootloader mode" >&2; exit 2;;
esac
exec idf.py "$@"
