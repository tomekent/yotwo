#!/bin/bash
# Publish an archived release on GitHub: creates (or updates) the release for
# tag v<version>, with that version's section of CHANGELOG.md as its notes and
# two downloads:
#   yotwo-<version>.bin        the update file (player's web page > Firmware)
#   yotwo-<version>-cable.zip  everything for flashing by cable
#
# Usage: tools/publish_release.sh <version> [<version> ...]
# The tag must already be pushed. Needs the GitHub CLI (gh), logged in.
# Publish oldest first: the last one published becomes "Latest".
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
rel="$root/firmware/releases"
repo="tomekent/yotwo"
[ $# -gt 0 ] || { echo "usage: tools/publish_release.sh <version> [...]" >&2; exit 2; }

for v in "$@"; do
  v="${v#v}"
  app="$rel/yotwo-$v.bin"
  [ -f "$app" ] || { echo "no $app" >&2; exit 1; }
  git -C "$root" ls-remote --exit-code --tags origin "refs/tags/v$v" >/dev/null \
    || { echo "tag v$v is not on GitHub: git push origin v$v" >&2; exit 1; }
  zip="$("$root/tools/cable_zip.sh" "$v")"
  notes="$(mktemp)"
  # The "## <version>" section of CHANGELOG.md, up to the next one.
  awk -v v="$v" '/^## / { on = ($2 == v); next } on' "$root/CHANGELOG.md" > "$notes"
  [ -s "$notes" ] || { echo "no '## $v' section in CHANGELOG.md" >&2; exit 1; }
  sha=$(shasum -a 256 "$app" | cut -d' ' -f1)
  cat >> "$notes" <<EOF

### Downloads

- **\`yotwo-$v.bin\`**: already running yotwo? Upload this on the player's web page (Settings > Firmware).
- **\`yotwo-$v-cable.zip\`**: first install, or flashing by cable. See [Getting started](https://github.com/$repo/blob/main/docs/getting-started.md).

SHA-256 of \`yotwo-$v.bin\`: \`$sha\`
EOF
  if gh release view "v$v" -R "$repo" >/dev/null 2>&1; then
    gh release edit "v$v" -R "$repo" --title "yotwo $v" --notes-file "$notes"
    gh release upload "v$v" -R "$repo" --clobber "$app" "$zip"
  else
    gh release create "v$v" -R "$repo" --verify-tag --title "yotwo $v" --notes-file "$notes" "$app" "$zip"
  fi
  rm -f "$notes"
  echo "published v$v"
done
