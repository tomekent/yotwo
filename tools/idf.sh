#!/bin/bash
# Run idf.py for the Yoto firmware with the ESP-IDF v5.5.2 environment loaded.
# The IDF Python environment on this machine was created with Python 3.14, so that
# interpreter has to be first on PATH when export.sh runs.
#
# Usage: tools/idf.sh build | size | menuconfig | ...
# Flashing is deliberately not wrapped: see docs/README.md ground rules.
set -e
cd "$(dirname "$0")/../firmware"
export PATH="/opt/homebrew/opt/python@3.14/libexec/bin:/opt/homebrew/opt/python@3.14/bin:$PATH"
. "${IDF_PATH:-$HOME/esp/v5.5.2/esp-idf}/export.sh" >/dev/null
case "$1" in
  flash|erase-flash|erase_flash|app-flash|bootloader-flash|encrypted-flash|efuse-*)
    echo "refusing '$1': flashing needs an explicit decision; run idf.py yourself" >&2; exit 2;;
esac
exec idf.py "$@"
