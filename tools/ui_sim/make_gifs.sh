#!/bin/bash
# Make the animations in docs/media/ (animated WebP) from the UI simulator (see gif.sh): each
# drawn on the blank player (docs/media/yoto_gen2_blank.png), plus transparent
# overlays (animated PNG) of the same in docs/media/overlay/.
# Each is a scripted walk through one part of the UI, at the player's real speed.
set -euo pipefail
cd "$(dirname "$0")/../.."
m=docs/media
mkdir -p $m/overlay
# make NAME <commands...>: docs/media/anim_NAME and docs/media/overlay/anim_NAME.png
make() {
  local name="$1"; shift
  tools/ui_sim/gif.sh "$m/anim_$name.webp" "$@"
  [ -n "${NO_OVERLAY:-}" ] || STYLE=overlay tools/ui_sim/gif.sh "$m/overlay/anim_$name.png" "$@"
}
W=t80   # past the start-up animation's minimum time

make startup say:Switching_on t100 boot t30 say:Ready:_the_books_played_last t60

make browse boot $W REC say:Turn_the_right_knob_to_browse t40 \
  r+ t80 r+ t80 r+ t80 r+ t60

make menu boot $W REC say:Hold_the_right_knob:_the_menu t15 rh t30 \
  say:Turn_through_the_menu r+ t25 r+ t25 r+ t25 r+ t30 \
  say:A-Z:_press,_then_pick_a_letter rp t30 r+ t20 r+ t30 \
  say:Press:_the_books_at_that_letter rp t90

make play boot $W REC say:Press_the_right_knob_to_play t20 r+ t20 rp t120 \
  say:Then_it_settles_on_the_cover t420 \
  say:Left_knob:_volume l+ t8 l+ t8 l+ t30 \
  say:Right_knob:_pick_a_chapter r+ t40 r+ t60 \
  say:Press_to_go_there rp t80

make favourite boot $W t60 REC say:Press_both_knobs:_a_favourite t20 both t60 \
  say:A_heart_marks_it_in_every_list t120 \
  say:Again_to_unfavourite both t50 t20

make cards boot $W REC say:A_book_card_plays_its_book t20 card:A t150 \
  say:Take_it_out_to_pause nocard t60 \
  say:A_sleep_card:_sleep_timer,_then_play card:S t120

make battery boot $W REC say:Tap_the_power_button pwr t90 \
  say:Plug_in_the_charger plug t90

# Sped up: one frame per 6 s of clock time.
make clock clockrun time:6:57 boot $W m1 every:150 REC say:Sunrise,_sped_up m19 \
  time:18:42 say:Sunset,_sped_up m19
