#!/bin/bash
# Record the UI simulator as an animation, at the player's own frame rate (one frame
# per 40 ms tick), with captions.
#
# Usage: [STYLE=device|flat|overlay] tools/ui_sim/gif.sh OUT <simulator commands...>
#   device   (default) GIF, or animated WebP if OUT ends .webp: the display lit up on the blank player
#            (docs/media/yoto_gen2_blank.png), caption underneath
#   flat     GIF: the bare 16 x 16 matrix on black, caption underneath
#   overlay  animated PNG with transparency: only the lit pixels, unlit ones and
#            the gaps clear, no caption; for laying over your own picture
#   Use say:TEXT (underscores for spaces) to caption what happens next, and tN to let
#   N ticks pass. Recording starts with the first command, or at a bare REC among
#   the commands (so set-up steps can run unrecorded).
# Example:
#   tools/ui_sim/gif.sh docs/media/anim_menu.gif boot t80 REC say:Hold_the_right_knob rh t40
# Needs ffmpeg (with drawtext).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
media="$here/../../docs/media"
style="${STYLE:-device}"
out="${1:?usage: gif.sh OUT <commands...>}"; shift
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

args=()
for a in "$@"; do [ "$a" = REC ] && args+=("rec:$tmp") || args+=("$a"); done
case " $* " in *" REC "*) ;; *) args=("rec:$tmp" "${args[@]}");; esac
"$here/run.sh" "${args[@]}" > /dev/null
n=$(ls "$tmp"/*.pam | wc -l | tr -d ' ')
[ "$n" -gt 0 ] || { echo "no frames recorded" >&2; exit 1; }

font=""
for f in /System/Library/Fonts/SFNS.ttf /System/Library/Fonts/Helvetica.ttc \
         /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf; do
  [ -f "$f" ] && { font="$f"; break; }
done

# One drawtext per caption, shown from its frame until the next caption's.
# $1 text colour, $2 size, $3 distance of the baseline area from the bottom.
captions() {
  local fc="" starts=() texts=() k from to
  [ -f "$tmp/captions.txt" ] && [ -n "$font" ] || return 0
  while IFS=$'\t' read -r frame text; do starts+=("$frame"); texts+=("$text"); done < "$tmp/captions.txt"
  for ((k = 0; k < ${#starts[@]}; k++)); do
    from=${starts[k]}; to=$n
    if (( k + 1 < ${#starts[@]} )); then to=$(( ${starts[k+1]} - 1 )); fi
    printf '%s' "${texts[k]}" > "$tmp/cap$k.txt"
    fc+=",drawtext=fontfile=$font:textfile=$tmp/cap$k.txt:fontcolor=$1:fontsize=$2:x=(w-text_w)/2:y=h-$3:enable='between(n,$from,$to)'"
  done
  printf '%s' "$fc"
}

frames=(-framerate 25 -i "$tmp/%05d.pam")
case "$style" in
  device)
    # The display sits in a 235 x 235 box at (122, 120) on the 480 x 462 mockup. Each
    # lit pixel is laid on the face, diffused.
    bg="$media/yoto_gen2_blank.png"
    # Diffused, as if shining through the face: the pixels softened by a blur, over two
    # glows in their own colour (a tight one and a wide one), and no hard edges. The
    # player itself is dimmed a little (its white surround keyed out first) so that
    # white light still stands out on its pale face, as a lit player does in a room.
    # Tunable: PX_BLUR PX_GAIN GLOW1 GLOW1_GAIN GLOW2 GLOW2_GAIN DIM.
    graph="[1:v]scale=235:235:flags=area,format=rgba,split=3[p0][g0][w0];"
    # (Blurred premultiplied, or the clear black around each pixel bleeds in as a dark rim.)
    blur() { echo "premultiply=inplace=1,gblur=sigma=$1,unpremultiply=inplace=1,lut=a='min(255,val*$2)'"; }
    graph+="[p0]$(blur ${PX_BLUR:-1.4} ${PX_GAIN:-0.88})[px];"
    graph+="[g0]$(blur ${GLOW1:-3} ${GLOW1_GAIN:-1.4})[glow];"
    graph+="[w0]$(blur ${GLOW2:-9} ${GLOW2_GAIN:-1.0})[wide];"
    graph+="color=c=white:s=480x502:r=25[page];[0:v]format=rgba,colorkey=0xFFFFFF:0.03:0.02,eq=brightness=-${DIM:-0.15}:gamma=0.92[dev];[page][dev]overlay=0:0[bg];[bg][wide]overlay=122:120[b0];[b0][glow]overlay=122:120[b1];[b1][px]overlay=122:120:shortest=1"
    graph+="$(captions 0x4a4a4a 17 30),fps=12.5"
    if [[ "$out" == *.webp ]]; then  # animated WebP: a fraction of a GIF's size for the soft glow
      ffmpeg -loglevel error -y -loop 1 -framerate 25 -i "$bg" "${frames[@]}" -filter_complex "$graph" \
        -c:v libwebp_anim -quality 75 -compression_level 6 -loop 0 "$out"
    else
      graph+=",split[a][b];[a]palettegen=max_colors=255:stats_mode=full[p];[b][p]paletteuse=dither=none:diff_mode=rectangle"
      ffmpeg -loglevel error -y -loop 1 -framerate 25 -i "$bg" "${frames[@]}" -filter_complex "$graph" -loop 0 "$out"
    fi
    ;;
  flat)
    graph="color=c=0x060606:s=264x294:r=25[bg];"
    graph+="[0:v]format=rgba[px];[bg][px]overlay=12:12:shortest=1"
    graph+="$(captions 0xC8C8C8 15 24),split[a][b];[a]palettegen=max_colors=128:stats_mode=full[p];[b][p]paletteuse=dither=none"
    ffmpeg -loglevel error -y "${frames[@]}" -filter_complex "$graph" -loop 0 "$out"
    ;;
  overlay)
    ffmpeg -loglevel error -y "${frames[@]}" -plays 0 -f apng -pix_fmt rgba "$out"
    ;;
  *) echo "unknown STYLE '$style' (device, flat or overlay)" >&2; exit 2;;
esac
echo "$out: $n frames, $(( n * 40 / 1000 )) s, $(( $(wc -c < "$out") / 1024 )) KB"
