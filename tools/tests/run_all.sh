#!/bin/sh
# Every desktop test. None needs the player or the ESP-IDF toolchain; a C compiler and
# python3 are enough (node, if present, also checks the setup page's script).
cd "$(dirname "$0")/../.."
status=0
tools/tests/ui_scenarios.sh || status=1
cc -std=c11 -w -O1 -Itools/ui_sim -Ifirmware/main/ui -o "${TMPDIR:-/tmp}/yotwo_ui_units" tools/tests/ui_units.c firmware/main/ui/gfx.c -lm \
  && "${TMPDIR:-/tmp}/yotwo_ui_units" || status=1
tools/tests/small_units.sh || status=1
tools/tests/favourites_scan.sh || status=1
tools/tests/setup_page.sh || status=1
tools/tests/web_units.sh || status=1
[ $status = 0 ] && echo "ALL PASSED" || echo "SOME FAILED"
exit $status
