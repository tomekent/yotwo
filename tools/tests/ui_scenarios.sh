#!/bin/sh
# Behaviour tests for the UI (firmware/main/ui/ui_core.c), run on the desktop through the
# simulator. Each case feeds it a script of knob, card, cable and clock events and checks
# what it asked the player to do (the "[host] ..." lines) or what it drew.
#
#   tools/tests/ui_scenarios.sh        exit status 0 if every case passes
cd "$(dirname "$0")/../.."
UI=firmware/main/ui
BIN="${TMPDIR:-/tmp}/yotwo_ui_sim_test"
cc -std=c11 -w -O1 -Itools/ui_sim -I"$UI" -o "$BIN" tools/ui_sim/sim.c "$UI/ui_core.c" "$UI/gfx.c" -lm || exit 2

pass=0; fail=0
# has NAME 'REGEX' cmds...    the output must contain a line matching REGEX
# hasnt NAME 'REGEX' cmds...  ...must not
# seq NAME 'A|B|C' cmds...    the host lines, joined by |, must contain this run in order
# noseq NAME 'A|B|' cmds...   ...must not
check() { if [ "$1" = ok ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: $2"; echo "$3" | sed 's/^/      /' | tail -12; fi; }
has()   { n=$1; re=$2; shift 2; out=$("$BIN" "$@" 2>&1); if echo "$out" | grep -Eq -- "$re"; then check ok; else check no "$n (wanted: $re)" "$out"; fi; }
hasnt() { n=$1; re=$2; shift 2; out=$("$BIN" "$@" 2>&1); if echo "$out" | grep -Eq -- "$re"; then check no "$n (unwanted: $re)" "$out"; else check ok; fi; }
noseq() { n=$1; bad=$2; shift 2; out=$("$BIN" "$@" 2>&1); got=$(echo "$out" | sed -n 's/^  \[host\] //p' | tr '\n' '|'); case "$got" in *"$bad"*) check no "$n (unwanted: $bad; got: $got)" "$out" ;; *) check ok ;; esac; }
# (seq_ leaves out the display's brightness changes unless the wanted run mentions them.)
hostrun() { if echo "$1" | grep -q brightness; then cat; else grep -v '^brightness '; fi; }
seq_()  { n=$1; want=$2; shift 2; out=$("$BIN" "$@" 2>&1); got=$(echo "$out" | sed -n 's/^  \[host\] //p' | hostrun "$want" | tr '\n' '|'); case "$got" in *"$want"*) check ok ;; *) check no "$n (wanted: $want; got: $got)" "$out" ;; esac; }

# --- starting to play
has   "left press after switching on resumes the last book" 'play "Charlie and the Great Glass Elevator"' boot t80 lp
has   "choosing a book plays it"                             'play "'                                   boot t80 r+ rp
seq_  "left press toggles play and pause"                    'pause|play|'                              boot t80 lp t40 lp t10 lp

# --- cards
seq_  "a book card plays its book; taking it out pauses"     'play "The Gruffalo"|pause|'               boot t80 card:A t40 nocard
seq_  "putting the card back resumes"                        'pause|resume|'                            boot t80 card:A t40 nocard t10 card:A
has   "a card put in before the library has loaded still plays" 'play "The Gruffalo"'                   card:A boot t80 t5
has   "an unknown card says NEW and is linked by both knobs" 'card X linked to'                         boot t80 lp t40 card:X both
seq_  "a sleep card plays, dims, and pauses when taken out"  'play "Charlie and the Great Glass Elevator"|brightness 3|pause|' boot t80 card:S t40 nocard
has   "a sleep card dims the display"                        'brightness 3$'                            boot t80 card:S t5
seq_  "taking the sleep card out restores the brightness"    'brightness 3|pause|brightness 80|'          boot t80 card:S t40 nocard t5
seq_  "...and putting it back dims it again"                 'brightness 3|pause|brightness 80|resume|brightness 3|' boot t80 card:S t40 nocard t5 card:S t5
seq_  "the sleep timer pauses at its end"                    'brightness 3|pause|'                      endless boot t80 card:S t40 m31
hasnt "...and not before"                                    'pause'                                    endless boot t80 card:S t40 m28
seq_  "the sleep timer is frozen while the card is out"      'pause|resume|pause|'                      endless boot t80 card:S t40 m10 nocard m40 card:S m21
noseq "...so the card back in gets the time that was left"   'resume|pause|'                            endless boot t80 card:S t40 m10 nocard m40 card:S m18
seq_  "after the timer has ended, out and in adds the extension" 'pause|resume|pause|'                  endless boot t80 card:S t40 m31 nocard t20 card:S m6
noseq "...of five minutes, not less"                         'resume|pause|'                            endless boot t80 card:S t40 m31 nocard t20 card:S m4
has   "sleep mode ends when play is pressed with the card out" 'brightness (80|20)$'                    boot t80 card:S t40 nocard t10 lp t5

# --- favourites
has   "both knobs marks the book shown as a favourite"       'favourite on'                             boot t80 both
seq_  "both knobs again unmarks it"                          'favourite on: Charlie and the Great Glass Elevator|favourite off: Charlie and the Great Glass Elevator|' boot t80 both t40 both

# --- the clock
has   "the clock comes up after 30 s idle"                   'W\.W\.\.\.W\.W|WWW' boot t80 time:12:34 t800 show
hasnt "the first touch on the clock only dismisses it"       'play "'             boot t80 time:12:34 t800 rp
has   "...except a left press, which also plays"             'play "'             boot t80 time:12:34 t800 lp
hasnt "a hold on the book page while playing does not pause" 'pause'              boot t80 time:19:20 lp t60 rh t400
has   "a hold on the book page without the time says WAIT"   'Y\.Y\.\.Y\.\.YYY\.YYY' boot t80 lp t60 rh show

# --- brightness, dimming, switching off
seq_  "day level, idle dim, touch restores, night level, idle dim" 'brightness 80|brightness 20|brightness 80|brightness 40|brightness 10|' boot t80 time:12:00 t1600 r+ time:21:00 t10 t1600
has   "left alone on battery it switches off after an hour"  'power off'          boot t80 m61
hasnt "not before the hour"                                  'power off'          boot t80 m58
hasnt "never on external power"                              'power off'          boot t80 plug m200
hasnt "never while a book plays"                             'power off'          endless boot t80 lp m200
has   "a touch starts the hour again"                        'power off'          boot t80 m40 r+ m61
hasnt "a touch starts the hour again (not early)"            'power off'          boot t80 m40 r+ m40
has   "an empty battery switches the player off"             'power off'          boot t80 bat:1 t300
hasnt "a low battery only warns"                             'power off'          boot t80 bat:8 t300
hasnt "an empty battery on the charger does not"             'power off'          boot t80 plug bat:1 t300

# --- settings
has   "SET > AUTO sets the switch-off time"                  'auto-off 120'       boot t80 rh r- rp r- r- r- r- rp r+ rp
has   "SET > CLK > RISE sets the sunrise"                    'clock setting 1 = 435' boot t80 rh r- rp r- r- r- r- r- r- rp r+ rp r+ rp
has   "SET > VOL sets the volume limit (default 80, in tens)" 'volume limit 90'    boot t80 rh r- rp r- r- r- rp r+ rp
has   "...and shows the current limit first"                 'W\.W\.WWW|WWW\.WWW' boot t80 rh r- rp r- r- r- rp show
has   "SET > BRT > DAY sets the day brightness (default 80)" 'bright setting 0 = 90' boot t80 rh r- rp r- r- r- r- r- rp rp r+ rp
has   "SET > BRT > NITE sets the night brightness (default 40)" 'bright setting 1 = 30' boot t80 rh r- rp r- r- r- r- r- rp r+ rp r- rp
has   "...the display shows the level as it is chosen"      'brightness 90$'                          boot t80 rh r- rp r- r- r- r- r- rp rp r+ t2
has   "SET > WIFI starts setup, and a hold leaves it"        'setup stopped'      boot t80 rh r- rp r- rp rh
has   "...and so does a press of either knob"              'setup stopped'      boot t80 rh r- rp r- rp lp
has   "...or the power button"                             'setup stopped'      boot t80 rh r- rp r- rp pwr
hasnt "...but not turning a knob"                         'setup stopped'      boot t80 rh r- rp r- rp r+ l+
has   "a hold on the start-up screen opens setup"            'setup started'      status:WIFI rh
hasnt "a hold elsewhere does not"                            'setup started'      boot t80 rh rh rh
has   "SET > LIB switches library"                           'library -> Grown-ups' boot t80 rh r- rp rp r+ rp

# --- the battery display
has   "a power-button touch shows the battery"               'G'                  boot t80 pwr show
has   "a scrolling message does not replace it"              'G'                  boot t80 pwr msg:HELLO_THERE show

# --- the copy-for-the-SD-card square, top left of a book. In the frames T is blue (arrived)
#     and w grey-blue (still to come); the cover beside it is V. Rows are indented by 4.
hasnt "no square when no copy is wanted or it is complete"   '^    (www|Tww|TTw|TTT)V'   boot t80 show
has   "wanted, not begun: all grey-blue"                     '^    wwwV'                 boot t80 dl:0 t1 show
hasnt "wanted, not begun: nothing blue"                      '^    (Tww|TTw|TTT)V'       boot t80 dl:0 t1 show
has   "half arrived: the bottom row is full"                 '^    TTTV'                 boot t80 dl:50 t1 show
has   "half arrived: one more in the middle row, from the left" '^    TwwV'              boot t80 dl:50 t1 show
hasnt "held: the next pixel does not flash"                  '^    TTwV'                 boot t80 dl:50 t1 show t15 show t15 show t15 show
has   "being fetched: the next pixel flashes"                '^    TTwV'                 boot t80 dl:50 dlrun t1 show t15 show t15 show t15 show
has   "nearly all arrived: eight of nine"                    '^    TTwV'                 boot t80 dl:95 t1 show
hasnt "gone once complete"                                   '^    (www|Tww|TTw|TTT)V'   boot t80 dl:50 t1 dl:-1 t1 show
# --- the Downloaded list, and offline mode (the simulator's books 0 and 5 have copies:
#     "Charlie and the Chocolate Factory" and "Room on the Broom")
has   "DOWN lists only the downloaded books: the first"      'play "Charlie and the Chocolate Factory"' boot t80 rh r+ r+ r+ rp rp
has   "...and the second, and no others (it wraps round)"    'play "Room on the Broom"'                 boot t80 rh r+ r+ r+ rp r+ rp
has   "...wrapping back to the first after two"              'play "Charlie and the Chocolate Factory"' boot t80 rh r+ r+ r+ rp r+ r+ rp
has   "Wi-Fi gone: the symbol with a red cross"              '\.\.R\.\.\.R|R\.\.\.R'         boot t80 wifi:0 t2 show
hasnt "Wi-Fi back: the symbol without the cross"             'R'                        boot t80 wifi:0 t100 wifi:1 t2 show
has   "Wi-Fi back: the symbol itself"                        'T\.\.\.\.TTT\.\.\.\.T'       boot t80 wifi:0 t100 wifi:1 t2 show
has   "offline, the home list is the downloaded books, on the one last played" 'play "Room on the Broom"' boot t80 wifi:0 t60 rp
has   "...and the other one is next, then round again"       'play "Charlie and the Chocolate Factory"' boot t80 wifi:0 t60 r+ rp
has   "offline, A-Z holds only downloaded books"             'play "Room on the Broom"'                 boot t80 wifi:0 t60 rh r+ r+ r+ r+ rp r+ rp rp
hasnt "offline, a book without a copy cannot be reached"     'play "(Matilda|Zog|Stick Man|The Gruffalo)' boot t80 wifi:0 t60 r+ rp r+ rp r+ rp r+ rp
has   "offline, left press carries on with a downloaded book" 'play "Room on the Broom"'                boot t80 wifi:0 t60 lp
has   "online, left press carries on with the last book whatever it is" 'play "Charlie and the Great Glass Elevator"' boot t80 lp
has   "Wi-Fi back: every book is on offer again"             'play "Charlie and the Great Glass Elevator"' boot t80 wifi:0 t60 wifi:1 t60 lp
hasnt "a book playing from the card is left alone when Wi-Fi goes" 'pause' endless boot t80 rh r+ r+ r+ rp rp t40 wifi:0 t100
# --- the LED ring: nightlight face-down, the wake light at sunrise (defaults: red, 50 %,
#     sunrise 7:00 over 15 min, held 10 min). Ring values are the final colour sent.
hasnt "upright: the ring stays dark"                         'ring [1-9A-F]'        boot t80 t100
has   "face-down: the nightlight, red at 50 %"               'ring 7F0000'          boot t80 face:1 t5
has   "face-down: the display is dark"                       '^    \.{16}$'          boot t80 face:1 t5 show
seq_  "picked up again: the ring goes off"                   'ring 7F0000|ring 000000|' boot t80 face:1 t5 face:0 t5
has   "SET > LITE > COL previews the colour on the ring"     'ring 7F2D00'          boot t80 rh r- rp r- r- rp r+ rp r+
has   "...and choosing it keeps it"                          'light setting 1 = 16734720' boot t80 rh r- rp r- r- rp r+ rp r+ rp
hasnt "the wake light is off unless turned on"               'ring [1-9A-F]'        boot t80 time:7:05 t5
has   "wake light: half-way up the sunrise, half the brightness, warm orange" 'ring 3F1A05' boot t80 wake:1 time:7:07:30 t5
has   "wake light: full once the sun is up"                  'ring 7F350A'          boot t80 wake:1 time:7:15:00 t5
has   "...and on whichever way up the player is"             'ring 7F350A'          boot t80 wake:1 face:0 time:7:20 t5
seq_  "...and off after the hold (10 min)"                   'ring 7F350A|ring 000000|' boot t80 wake:1 time:7:20 t5 time:7:25:01 t5
seq_  "a button during the wake light turns it off"          'ring 7F350A|ring 000000|' boot t80 wake:1 time:7:20 t5 rp t5
noseq "...and that press does nothing else"                  'play "'               boot t80 wake:1 time:7:20 t5 lp t5
noseq "...and it stays off that morning"                     'ring 000000|ring 7F350A|' boot t80 wake:1 time:7:20 t5 rp t5 time:7:21 t5
has   "...but comes back the next morning"                   'ring 3F1A05'          boot t80 wake:1 time:7:20 t5 rp t5 time:23:00 t5 time:7:07:30 t5
echo "ui scenarios: $pass passed, $fail failed"
[ "$fail" = 0 ]
