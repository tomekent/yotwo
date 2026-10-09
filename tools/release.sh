#!/bin/bash
# Archive the current build into firmware/releases/.
#
# Copies firmware/build/yotwo.bin to releases/yotwo-<version>.bin and
# releases/yotwo-latest.bin, and the three boot files a cable flash also
# needs (bootloader, partition table, blank otadata) to releases/boot/.
# The version comes from the image itself and must match firmware/version.txt,
# so a stale build can't be archived under a new name.
#
# Before archiving it runs every desktop test (tools/tests/run_all.sh) and refuses
# if any fails; warns when the app is getting close to its 3.75 MB slot; and says
# loudly if the bootloader or partition table differ from the ones in
# releases/boot/ (that is, from what the player has): such a build must go on by
# cable, because an update over Wi-Fi replaces the app alone.
#
# Usage: tools/release.sh [--skip-tests]   (after tools/idf.sh build)
# Then add the printed row to firmware/releases/RELEASES.md and tag v<version>.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/firmware/build"
rel="$root/firmware/releases"
skip_tests=0
[ "${1:-}" = "--skip-tests" ] && skip_tests=1

app_version() {  # version string from the image's esp_app_desc_t
  python3 -c '
import sys, struct
d = open(sys.argv[1], "rb").read(80)
if struct.unpack("<I", d[32:36])[0] != 0xABCD5432:
    sys.exit("not an ESP-IDF app image: " + sys.argv[1])
print(d[48:80].split(b"\0")[0].decode())' "$1"
}

want="$(tr -d '[:space:]' < "$root/firmware/version.txt")"
have="$(app_version "$build/yotwo.bin")"
if [ "$want" != "$have" ]; then
  echo "build is $have but version.txt says $want: rebuild first" >&2
  exit 1
fi

if [ $skip_tests = 0 ]; then
  echo "running the desktop tests..."
  if ! "$root/tools/tests/run_all.sh" > "${TMPDIR:-/tmp}/yotwo_release_tests.log" 2>&1; then
    grep -E "FAIL|SOME FAILED" "${TMPDIR:-/tmp}/yotwo_release_tests.log" | head -20 >&2
    echo "tests failed (full output in ${TMPDIR:-/tmp}/yotwo_release_tests.log): not archived" >&2
    exit 1
  fi
  echo "tests passed"
fi

slot=$((0x3C0000))
size=$(stat -f %z "$build/yotwo.bin")
pct=$((size * 100 / slot))
[ $pct -ge 90 ] && echo "WARNING: the app fills $pct% of its slot" >&2

# The bootloader carries its own build time, so a rebuild with no real change still
# differs: compare its segments with that field blanked, not the file.
cable=$(python3 - "$build" "$rel/boot" <<'PY'
import sys, os
build, boot = sys.argv[1], sys.argv[2]
def body(path):
    d = open(path, "rb").read()
    off = 24
    for _ in range(d[1]):  # segment count; each is an 8-byte header and its data
        off += 8 + int.from_bytes(d[off + 4:off + 8], "little")
    b = bytearray(d[:off])  # without the trailing checksum and hash, which cover the time
    if b[32] == 0x50:       # esp_bootloader_desc_t: date_time at 72..96
        b[72:96] = bytes(24)
    return bytes(b)
changed = []
pairs = [("bootloader", "bootloader/bootloader.bin", "bootloader.bin", body),
         ("partition table", "partition_table/partition-table.bin", "partition-table.bin", lambda p: open(p, "rb").read())]
for name, new, old, read in pairs:
    old = os.path.join(boot, old)
    if os.path.exists(old) and read(os.path.join(build, new)) != read(old):
        changed.append(name)
print(" and ".join(changed))
PY
)
if [ -n "$cable" ]; then
  echo "" >&2
  echo "!!! The $cable differ from releases/boot/ (what the player has)." >&2
  echo "!!! This build must go on BY CABLE (tools/flash.sh), not over Wi-Fi." >&2
  echo "" >&2
fi

out="$rel/yotwo-$want.bin"
if [ -e "$out" ] && ! cmp -s "$build/yotwo.bin" "$out"; then
  echo "$out already exists with different contents; bump the version" >&2
  exit 1
fi

mkdir -p "$rel/boot"
cp "$build/yotwo.bin" "$out"
cp "$build/yotwo.bin" "$rel/yotwo-latest.bin"
cp "$build/bootloader/bootloader.bin" "$build/partition_table/partition-table.bin" \
   "$build/ota_data_initial.bin" "$rel/boot/"

sha=$(shasum -a 256 "$out" | cut -d' ' -f1)
echo "archived $out (and yotwo-latest.bin, boot/)"
echo "RELEASES.md row:"
echo "| $want | $(date +%Y-%m-%d) | (this commit) | $size B | $sha | ${cable:+CABLE ONLY ($cable changed). }<notes> |"
