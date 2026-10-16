#!/bin/bash
# Record the UI simulator as an animated GIF, at the player's own frame rate
# (one frame per 40 ms tick), with captions under the display.
#
# Usage: tools/ui_sim/gif.sh OUT.gif <simulator commands...>
#   Use say:TEXT (underscores for spaces) to caption what happens next, and tN
#   to let N ticks pass. Recording starts with the first command, or at a
#   bare REC among the commands (so set-up steps can run unrecorded).
# Example:
#   tools/ui_sim/gif.sh docs/media/anim_menu.gif boot t25 say:Press_right_knob rp t20 ...
# Needs ffmpeg (with drawtext).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="${1:?usage: gif.sh OUT.gif <commands...>}"; shift
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

args=()
for a in "$@"; do [ "$a" = REC ] && args+=("rec:$tmp") || args+=("$a"); done
case " $* " in *" REC "*) ;; *) args=("rec:$tmp" "${args[@]}");; esac
"$here/run.sh" "${args[@]}" > /dev/null
n=$(ls "$tmp"/*.ppm | wc -l | tr -d ' ')
[ "$n" -gt 0 ] || { echo "no frames recorded" >&2; exit 1; }

font=""
for f in /System/Library/Fonts/SFNS.ttf /System/Library/Fonts/Helvetica.ttc \
         /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf; do
  [ -f "$f" ] && { font="$f"; break; }
done

# One drawtext per caption, shown from its frame until the next caption's.
filters="pad=iw:ih+30:0:0:color=0x060606"
if [ -f "$tmp/captions.txt" ] && [ -n "$font" ]; then
  starts=(); texts=()
  while IFS=$'\t' read -r frame text; do starts+=("$frame"); texts+=("$text"); done < "$tmp/captions.txt"
  for ((k = 0; k < ${#starts[@]}; k++)); do
    from=${starts[k]}; to=$n
    if (( k + 1 < ${#starts[@]} )); then to=$(( ${starts[k+1]} - 1 )); fi
    printf '%s' "${texts[k]}" > "$tmp/cap$k.txt"
    filters+=",drawtext=fontfile=$font:textfile=$tmp/cap$k.txt:fontcolor=0xC8C8C8:fontsize=15:x=(w-text_w)/2:y=h-24:enable='between(n,$from,$to)'"
  done
fi

ffmpeg -loglevel error -y -framerate 25 -i "$tmp/%05d.ppm" \
  -filter_complex "[0:v]$filters,split[a][b];[a]palettegen=max_colors=128:stats_mode=full[p];[b][p]paletteuse=dither=none" \
  -loop 0 "$out"
echo "$out: $n frames, $(( n * 40 / 1000 )) s, $(( $(wc -c < "$out") / 1024 )) KB"
