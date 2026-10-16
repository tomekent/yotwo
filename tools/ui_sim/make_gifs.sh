#!/bin/bash
# Make the animated GIFs in docs/media/ from the UI simulator (see gif.sh).
# Each is a scripted walk through one part of the UI, at the player's real speed.
set -euo pipefail
cd "$(dirname "$0")/../.."
g=tools/ui_sim/gif.sh
m=docs/media
W=t80   # past the start-up animation's minimum time

$g $m/anim_startup.gif say:Switching_on t100 boot t30 say:Ready:_the_books_played_last t60

$g $m/anim_browse.gif boot $W REC say:Turn_the_right_knob_to_browse t40 \
  r+ t80 r+ t80 r+ t80 r+ t60

$g $m/anim_menu.gif boot $W REC say:Hold_the_right_knob:_the_menu t15 rh t30 \
  say:Turn_through_the_menu r+ t25 r+ t25 r+ t25 r+ t30 \
  say:A-Z:_press,_then_pick_a_letter rp t30 r+ t20 r+ t30 \
  say:Press:_the_books_at_that_letter rp t90

$g $m/anim_play.gif boot $W REC say:Press_the_right_knob_to_play t20 r+ t20 rp t120 \
  say:It_settles_on_the_cover_and_chapter t240 \
  say:Left_knob:_volume l+ t8 l+ t8 l+ t30 \
  say:Right_knob:_pick_a_chapter r+ t40 r+ t60 \
  say:Press_to_go_there rp t80

$g $m/anim_favourite.gif boot $W t60 REC say:Press_both_knobs:_a_favourite t20 both t60 \
  say:A_heart_marks_it_in_every_list t120 \
  say:Again_to_unfavourite both t50 t20

$g $m/anim_cards.gif boot $W REC say:A_book_card_plays_its_book t20 card:A t150 \
  say:Take_it_out_to_pause nocard t60 \
  say:A_sleep_card:_sleep_timer,_then_play card:S t120

$g $m/anim_battery.gif boot $W REC say:Tap_the_power_button pwr t90 \
  say:Plug_in_the_charger plug t90

# Sped up: one frame per 6 s of clock time.
$g $m/anim_clock.gif clockrun time:6:57 boot $W m1 every:150 REC say:Sunrise,_sped_up m19 \
  time:18:42 say:Sunset,_sped_up m19
