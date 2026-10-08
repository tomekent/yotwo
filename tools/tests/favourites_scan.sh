#!/bin/sh
# Desktop test of the JSON scanner in firmware/main/favourites.c (the part that reads the
# server's collection replies). Extracts the scanner from the source and runs it against
# sample replies with nested ids, escaped quotes and brackets inside strings.
set -e
cd "$(dirname "$0")/../.."
OUT="${TMPDIR:-/tmp}/yotwo_favscan"
{
  printf '%s\n' '#include <stdio.h>' '#include <string.h>' '#include <strings.h>' '#include <stdbool.h>' '#include <stddef.h>' \
                '#define ID_LEN 40' '#define COLLECTION "Favourites"'
  sed -n '/^typedef void (\*element_cb)/,/^\/\* Talking to the server/p' firmware/main/favourites.c | sed '$d'
  cat tools/tests/favourites_scan_main.c
} > "$OUT.c"
cc -w -o "$OUT" "$OUT.c"
"$OUT"
