#!/bin/sh
# Build and run the UI simulator. See sim.c for the commands.
set -e
cd "$(dirname "$0")"
UI=../../firmware/main/ui
BIN="${TMPDIR:-/tmp}/yotwo_ui_sim"
cc -std=c11 -Wall -Wextra -O1 -I. -I"$UI" -o "$BIN" sim.c "$UI/ui_core.c" "$UI/gfx.c" -lm
exec "$BIN" "$@"
