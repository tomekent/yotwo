# Journal

How yotwo was built, step by step: what was tried, what came back, and what
was learned. Newest entries at the bottom. It was written as the work went
along (largely by Claude, the AI assistant the firmware was built with), so
"the user" is the author and "I" is Claude.

The earliest entries, before 5 October 2026, covered taking a backup of the
original firmware and studying it to learn how the board is wired. Those
notes are not published: they describe Yoto's software rather than this
project. What came out of them that matters for yotwo (the pin map, the
chips, and how to drive them) is in [hardware.md](hardware.md) and in the
driver code under `firmware/main/hardware/`.

Serial logs named in the entries are kept privately: they contain the home
network name, server address and account name.

## 2026-10-05 — Direction set: Audiobookshelf player

The user clarified the goal: not a replica of open-yoto, but their own
UI/UX whose primary purpose is to connect to an Audiobookshelf server and
play audiobooks directly from it. Recorded with the
consequences for the plan, a first read of the ABS API (untested), an
architecture suggestion (own ESP-IDF project using ESP-ADF for audio) and
the UX decisions that are still open. Roadmap phases 3 and 5 rewritten.

## 2026-10-05 — Existing player and UI design located

The user pointed to an existing, fully functioning ABS player of theirs,
and mentioned a Claude Design project for the UI.

- The player is [ABSpresso](https://github.com/tomekent/ABSpresso),
  v0.1.0, ESP-IDF v5.5.2 for an ESP32-S3 round touchscreen board. Read its README, build config, board interface,
  player and API client headers.
- The UI design is the Claude Design canvas "Yoto 16x16 Screen Mockups"
  (two artboards: screen vocabulary and an interactive two-knob navigation
  prototype). Read both artboards.

Written up (in planning notes not published here) and the roadmap updated: the Yoto firmware is now planned as a port of
ABSpresso with a new 16 × 16 UI, not an ESP-ADF project. Nothing in either
of the user's other projects was modified.

## 2026-10-05 — Firmware started; first build

The user chose option 1 (new firmware in this repo, copying ABSpresso's
network and app code) and added three requirements: setup must work on a
16 × 16 display; playback should be card-driven like a stock Yoto (cached
on SD, else fetched from the server); and audio conversion has to be
handled. They also confirmed the design canvas link (the one already
read).

Work done (details in [firmware.md](firmware.md)):

- `tools/sync_abspresso.sh` copies 17 files (3,570 lines) from ABSpresso
  commit `8a203d2` into `firmware/main/abspresso/`, unmodified.
- New Yoto board layer in `firmware/main/hardware/`: pin map, IO expander,
  ES8388 + I2S audio, HT16D35x display, SD over SPI, battery, power.
- New `firmware/main/ui/`: 3 × 5 font renderer (glyphs from the design
  canvas) and a minimal UI.
- `tools/idf.sh` wrapper. ESP-IDF's `export.sh` failed at first: the
  Python environment was created with Python 3.14 and the shell's default
  is 3.9. The wrapper puts 3.14 first on the path. It refuses flash
  commands.
- **Build 1 failed at link:** `.iram0.text` overflowed `iram0_0_seg` by
  5,308 bytes. Fixed in `sdkconfig.defaults` by moving Wi-Fi's receive path
  and FreeRTOS functions to flash and disabling PSRAM bank switching.
  Also removed a non-existent Kconfig symbol for flash baud rate.
- **Build 2 succeeded:** 1.84 MB, IRAM 110 KB / 131 KB.

Observation: ABSpresso's setup password is already 8 digits, so it fits
the display as two lines of four with no change to the portal code.

Nothing was flashed. Nothing in the ABSpresso repo or the design canvas
was changed.

## 2026-10-05 — Input and the two-knob UI

User decisions: card linking by both URL and UID; removing a card pauses;
cards are lower priority than browsing the server's books with the knobs;
idea of both-knobs to write a bookmark or favourite to a card.

- `hardware/input.c`: encoders on the pulse counter, buttons polled
  through the IO expander, gesture detection.
- `ui/ui_core.c`: the navigation model from the design canvas as a
  hardware-independent state machine drawing through `gfx`. `ui/ui.c`
  connects it to ABSpresso's player and catalogue and runs the UI task.
- `tools/ui_sim/`: the same UI code compiled for the desktop with a fake
  library. Walked through start-up, setup, book list, play, chapter skip,
  volume, pause, menu, chapter list and sleep timer; frames looked right.
  One sample strip saved as `docs/media/ui_sim.png` (converted from PPM
  with a throwaway Pillow via `uv run --with pillow`).
- Firmware rebuilt: 1.86 MB, no warnings in our code.

Still nothing flashed.

## 2026-10-05 — First flash and first boot

The user said to flash, with the board in bootloader mode.

1. Re-ran `tools/verify_dump.py`: the backup still matched the chip in all
   eight regions.
2. Erased `0x9000`–`0xFFFF` (the start of the stock NVS, which is where our
   NVS and phy_init partitions now sit) so our firmware would start clean:
   `esptool.py … erase_region 0x9000 0x7000`.
3. Flashed at 115200 with no reset before or after:
   `esptool.py --chip esp32 --port /dev/cu.usbserial-1130 --baud 115200
   --before no_reset --after no_reset write_flash --flash_mode dio
   --flash_freq 40m --flash_size 8MB 0x1000 bootloader/bootloader.bin
   0x8000 partition_table/partition-table.bin 0x10000 yotwo.bin`.
   All three hashes verified; the app took 120 s. (A first attempt did
   nothing: zsh does not word-split a command held in a variable.)
4. The user reset the player; serial captured to
   `logs/yotwo_first_boot_2026-10-05.log`.

**It booted first time.** Results are tabulated in
[firmware.md](firmware.md#hardware-results). Highlights: 8 MB PSRAM found
(4 MB mapped); IO expander and ES8388 both answered on I2C at the expected
addresses; setup network came up and showed its password; the user joined
it from a phone, the player joined the home Wi-Fi and reached the
Audiobookshelf server over HTTPS; the first sign-in was rejected (HTTP
401).

Problems found: the SD card did not mount (DMA channel clash between the
two SPI buses; fixed in source, not yet reflashed); two harmless pull-up
errors for GPIO 35 from the pulse counter.

The stock firmware is no longer on the device. The later serial capture of
the setup session is kept locally but git-ignored because it contains the
home network name, server address and account name.

## 2026-10-05 — Setup completed, playback crash, second flash

From the serial capture of the setup session (kept locally, git-ignored):

- Second sign-in attempt succeeded; the player saved its settings and
  restarted, joined Wi-Fi, found 4 libraries and loaded 242 items.
- The user chose a book with the knobs. The session opened (28 chapters),
  the player began fetching segment 0, and the firmware **crashed with
  `LoadStoreError`**, restarted, and did the same again with another book.
- Decoded the backtrace with `xtensa-esp32-elf-addr2line` against the
  current ELF (the frames were coherent: `memcpy` ← `esp_http_client_read`
  ← `abs_stream_read` ← `fetch_segment` ← `fetch_task`). The destination
  address `0x4009b074` is in the IRAM heap region listed in the boot log
  (`At 4009AD78 len 00005288 (20 KiB): IRAM`). Cause and workaround are
  written up in [firmware.md](firmware.md#hardware-results).

The user put the board in bootloader mode and asked for the SD fix to be
flashed. Stopped the background serial capture (it was holding the port;
killing the wrapper shell left the Python process, which had to be killed
separately). Added `reserve_iram_heap()` to `main.c`, rebuilt, and flashed
bootloader, partition table and app without erasing settings. Hashes
verified.

## 2026-10-05 — Second build running: SD mounts, playback streams

Serial log after the second flash (`logs/yotwo_setup_2026-10-05_b.log`,
git-ignored): IRAM heap reserved (20,288 bytes), SD card mounted, library
loaded, two books opened from the knobs, streams decoded at 44.1 kHz
stereo, several chapter seeks, no crashes. Results table in
[firmware.md](firmware.md#hardware-results). Waiting on the user for what
they heard and saw.

## 2026-10-05 — Sound confirmed

The user reports that sound works and playing a book works, with an SD
card fitted. That confirms the audio path end to end (ES8388 init
sequence, I2S pin map, MCLK, amplifier enable). Not yet reported: display
orientation and colours, which knob is which and their direction, and
power-off behaviour.

## 2026-10-05 — ABSpresso fix and chapter titles

At the user's request, edited the ABSpresso repo
(`~/ESP32/Esp32-1.85-touch`, left uncommitted there):

- `main/app/player.c`: the four `MALLOC_CAP_INTERNAL` buffer allocations
  now use `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`.
- `main/app/player.c`, `player.h`: added `player_get_chapter()`; the
  session is freed under `s_lock` so the accessor cannot read freed
  chapters.

Re-ran `tools/sync_abspresso.sh` (SOURCE.txt records the same commit with
2 modified files). In our firmware the chapter list now shows the
highlighted chapter's title, scrolling, above `n/N`; checked in the
simulator. `reserve_iram_heap()` kept as a guard. Firmware rebuilt
(1.86 MB). Not yet flashed.

## 2026-10-05 — Local patch; library switching; browsing views

User decisions: keep the ABSpresso changes isolated to this repo; add
library switching to the top menu; browse by author, title, series,
recently played and recently added.

- Reverted `player.c` / `player.h` in the ABSpresso working tree
  (`git checkout`); its only remaining modification is the user's own
  `.gitignore` change.
- `firmware/main/abspresso/local.patch` now carries our changes (11 hunks
  in 4 files). `tools/sync_abspresso.sh` applies it after copying and has
  `--update-patch` to regenerate it. A fresh sync was compared against the
  committed files: identical.
- Added a `series` field to the copied `abs_api` (from `seriesName`).
- `ui_core.c` rewritten around views: an ordering of book indices plus
  optional groups, built with `qsort`. New screens: `BOOK` view chooser,
  group list, library list. `LIB` requests go through the main loop, which
  loads the library (cache first, then network) as ABSpresso's does.
- Simulator given a second library, authors, series and dates; walked
  through all five views, a series play and a library switch.
- Two slips caught by building: `mode_t` clashes with a system type on
  macOS (renamed `screen_t`); a `snprintf` buffer the ESP-IDF build
  rejected as too small.
- Firmware builds (1.86 MB). **Not flashed yet**: the player is still on
  the previous build.

## 2026-10-05 — Third flash; ghost presses traced to missing pull-ups

Flashed the browsing/library build (app only, settings kept). The user
reported ghost presses switching between menus and doubted that hold
worked.

- The log had bursts of "both knobs pressed" and one library switch.
- Re-read the original firmware's expander setup. The warning string is
  "ignoring any IOX pull/intmask register **errors**", not "ignoring the
  pull settings" as I had taken it from a truncated string earlier: the
  stock firmware writes pull enable and pull select for both ports and
  merely tolerates failure. Our driver wrote neither, leaving the knob
  buttons floating.
- Fix: `iox_init` now writes `0x46`/`0x47` = `0x30`/`0xA8` and
  `0x48`/`0x49` likewise, logging a warning if the chip rejects them.
- Also: every input event is now logged (to prove the controls), and the
  task watchdog timeout is 15 s after it fired once during a TLS handshake
  (backtrace decoded: `mbedtls_ecp_mul` under `abs_close_session`).
- Rebuilt (1.86 MB). Not yet flashed.

## 2026-10-05 — Pull-up fix confirmed; audio stutter reported

- Two serial captures after the fourth flash recorded nothing (35 minutes
  in total). The adapter was later found to be absent from `/dev`, so it
  had been unplugged; the player itself was fine.
- The user reset the player and reports **button pressing is fixed**.
- New report: **audio occasionally stutters or sounds clipped.**
- No log to diagnose from, so prepared a build that measures and also
  tries the likeliest causes: ABSpresso's `PLAYER_STATUS_LOG` statistics
  switched on from our CMake file (no change to the copied code); I2S DMA
  buffer 8 × 1023 frames; `slot_mask` both for mono; per-chip dirty check
  in `display_push`. Built (1.86 MB), not yet flashed.

## 2026-10-05 — Settled player screen with cover art; fifth flash

The user asked for the player screen to settle, after scrolling the name
and chapter, into a static screen: cover art if it can be made to work at
16 × 16, with the chapter number overlaid and progress shown.

- `main/cover16.c`: server-resized JPEG → linear-light box average to
  16 × 16, saturation and level lift, SD cache. Details in
  [firmware.md](firmware.md#cover-art-on-16--16).
- `ui_core.c`: the title/chapter scroll is now an introduction that plays
  once per book and per chapter; then the cover with an outlined chapter
  number and a bottom-row progress bar. Fallback without a cover.
- Simulator given a synthetic cover; layout checked
  (`docs/media/ui_player_settled.png`).
- Built (1.87 MB) together with the audio-investigation changes from the
  previous entry.
- The serial adapter had been re-plugged and now appears as
  `/dev/cu.usbserial-130` (was `-1130`); the first flash attempt failed on
  the old name. Flashed through the new one; MAC and hashes verified.

## 2026-10-05 — Mono regression found and fixed in source

The user reset the player and reported the audio was "awful, stuttery and
unrecognisable, but only for BFG", and that covers look decent.

- Log (`logs/yotwo_setup_2026-10-05_f.log`): The BFG is a mono stream. Its
  once-a-second statistics show the position advancing about 0.5 s per
  second; the two stereo books advance 1.0 s per second with no underruns
  or decode errors. So mono played at half speed.
- Cause: in the previous build I set `slot_mask = I2S_STD_SLOT_BOTH` with
  the channel still in `I2S_SLOT_MODE_MONO`. My error.
- Fix: I2S is always configured stereo; `board_audio_write()` duplicates
  mono samples into both channels in 512-sample blocks.
- One cover (4.7 KB JPEG) failed in `esp_jpeg` with error 8; the fallback
  screen covers it. Retry interval for failed covers raised to 10 minutes.
- Built (1.87 MB). Not yet flashed.

## 2026-10-05 — Cover-art book lists

The user asked for a cover-art view: artwork only while scrolling, then
after a moment the title and author scrolling across the bottom.

- Applied to every book list (recent, new, A–Z, and the books under an
  author or series) rather than as a separate view; text remains the
  fallback while a cover loads or if there is none. This was my reading of
  the request and is easy to change.
- `cover16.c`: eight-entry in-memory cache keyed by item ID.
- `ui_core.c`: `draw_book()`; caption starts 12 frames after the last
  turn, on a black band over rows 9–15.
- Checked in the simulator (`docs/media/ui_cover_list.png`). Built
  (1.87 MB) with the mono fix. Not yet flashed.

## 2026-10-05 — Library cache checked; whole-library cover cache; sixth flash

The user asked for more than eight covers to be retained, for the library
caches on the SD card to be made to work, including 16 × 16 covers for
every book when a library loads, and said to flash.

- **Library cache: already working.** The previous session's log shows the
  book list coming from the SD card 4.2 s after power-on
  (`showing 158 items … from the SD cache`) and the libraries list
  likewise, before the network refresh. No change needed; documented in
  [firmware.md](firmware.md#library-cache-on-the-sd-card).
- **Covers:** `cover16.c` rewritten. Memory cache raised from 8 to 320
  covers. New `cover16_prefetch()`, called from `ui_set_books()`, queues
  every book of the library; the task serves the cover the UI is waiting
  for first, then works through the queue, SD card first, server
  otherwise. Undecodable covers are remembered on the card as 1-byte
  files.
- Built (1.87 MB) and flashed (app only) through `/dev/cu.usbserial-130`;
  hash verified. This build also carries the mono fix and the cover-art
  lists.

## 2026-10-05 — Card lists, alphabet for A–Z, VIEW setting

User requests: the caption should scroll once and then return to the full
artwork; the layout should be the same with no artwork (caption along the
bottom) and apply to the other lists too; A–Z should go through the
alphabet first and drop into the list on choosing a letter; cover versus
text should be configurable.

- `ui_core.c`: one `draw_card()` for every list. Groups are pictured by
  their first book's cover; chapters by the playing book's cover with the
  number on it; libraries have a placeholder.
- `V_TITLE` is now grouped by first letter of the sort title. Unlike
  authors and series, choosing a letter shows the *whole* title list
  positioned at that letter, and hold returns to the alphabet at the
  letter reached.
- New menu item `VIEW` (`ART` / `TEXT`), stored in NVS namespace `ui`.
- `gfx`: 2× scaled text for the big letter; glyphs for `#`, `&`, `+`.
- Simulator walk-through saved as `docs/media/ui_views.png`.
- From the running player's log: the cover pre-fetch finished (172 from
  the server in about ten minutes, 3 undecodable); stereo audio statistics
  clean over 354 s.
- Built (1.88 MB). Not yet flashed: the player is on the sixth build.

## 2026-10-05 — Many covers fail to decode; decoder switched

The full log of the sixth build's session showed a second pre-fetch
summary I had missed: for the kids' library, `4 from the SD card, 75 from
the server, 52 without a usable cover`. Counting the errors: 70 ×
`esp_jpeg` error 8 (`JDR_FMT3`, unsupported JPEG standard) and 3 × error 6,
on images of 2–8 KB with all-different sizes. So about a third of that
library's covers were rejected, not the handful I told the user.

`esp_jpeg` defaults to the TJpgDec in the ESP32's mask ROM
(`CONFIG_JD_USE_ROM=y`). Set it to `n` to use the newer decoder shipped in
the component. The "no usable cover" marker on the SD card is now two
bytes with a version, so the 55 markers already written are ignored and
those covers re-tried. Built (1.88 MB). **Untested**: it is a plausible
fix, not a confirmed one.

## 2026-10-05 — Seventh flash

The user put the player in bootloader mode and said to flash. Flashed
bootloader, partition table and app (1.88 MB) through
`/dev/cu.usbserial-130`; hashes verified; settings kept. This build has the
card-style lists, the alphabet for A–Z, the `VIEW` setting, the bundled
JPEG decoder and the mono fix (still unconfirmed). Serial capture started.

## 2026-10-05 — One menu, one list format

User feedback on the seventh build: everything asked about works (lists,
A–Z, captions, `VIEW`, The BFG). Requests: one format for every list and
item with the text along the bottom; no numbering but keep the position
line, with more contrast or a coloured marker; lists should wrap round in
every menu; and the two menus (BOOK/CHAP/BED/LIB, then LAST/NEW/A-Z/AUTH/
SERI) are confusing.

- `ui_core.c` reassembled: the two menus are merged into one of nine
  items; `M_BROWSE` and the `n/N` counters are gone; `draw_card()` draws
  every list and menu; `draw_line()` is the position line (cyan marker,
  near-black track, thumb width scales with item count); `wrapi()` for
  wrap-around. Small icons for menu items and a book placeholder.
- Layout: caption band rows 8–14 (text rows 9–13), position line row 15.
- Simulator: `docs/media/ui_unified.png`.
- From the seventh build's log: bundled JPEG decoder decodes every cover;
  watchdog warnings at start-up traced again to TLS maths, so the build is
  now `-O2` (`CONFIG_COMPILER_OPTIMIZATION_PERF`).
- Built (1.82 MB). Not yet flashed.

## 2026-10-05 — Eighth flash

The user put the player in bootloader mode and said to flash. Flashed
bootloader, partition table and app (1.82 MB, the first `-O2` build)
through `/dev/cu.usbserial-130`; hashes verified; settings kept. Carries
the single menu, unified list format, position line and wrap-around.
Serial capture started.

## 2026-10-05 — Caption-first covers; favourites

User requests: in the cover view the title and author should be on the
bottom row while scrolling through, starting with the start of the text in
view and then scrolling, with the full artwork after it has finished; and
favourites: both knobs toggles a book as a favourite, a menu item lists
them, and a favourite is marked at the top right. (The message ended
mid-sentence, "indicated on the top right of"; taken to mean the cover.)

- `draw_card()`: with art, the band and caption appear at once from the
  first letter, hold for 14 frames, scroll off, then the whole picture.
- Favourites: `V_FAV` view and `FAV` menu item with a heart icon;
  `toggle_favourite()` on the both-knobs event; 3 × 3 heart on a black
  patch at the top right of book cards and the settled player screen.
  Stored as item IDs in an NVS blob (`ui`/`favs`, up to 100).
- Simulator: `docs/media/ui_favourites.png`.
- Eighth build's log: no watchdog warnings with `-O2`; one underrun after
  a far seek while the server re-transcoded.
- Built (1.83 MB). Not yet flashed.

## 2026-10-05 — Ninth flash

The user put the player in bootloader mode and said to flash. Flashed the
app (1.83 MB) through `/dev/cu.usbserial-130`; hash verified; settings
kept. Carries the caption-first cover cards and favourites. Serial capture
started. The remainder of the eighth build's log was clean (no crashes, no
watchdog warnings).

## 2026-10-05 — Heart animation for favourites

Ninth build's log: three both-knob presses, each a clean single `both`
event; two favourites made and one removed; no crashes.

User request: confirm a favourite with a filled heart that pulses larger
briefly, and an unfavourite with an empty heart that shrinks slightly.

- `ui_core.c`: `OV_HEART` overlay replaces the `FAV+` / `FAV-` words.
  Three heart sizes (9, 11, 13 pixels wide); the empty heart is the
  outline of the same shapes, computed rather than drawn separately.
  Favourite: normal, large for frames 4–7, normal. Unfavourite: normal
  outline, then small from frame 6. 16 frames in all.
- Simulator: `docs/media/ui_heart_animation.png`. Built. Not yet flashed.

## 2026-10-05 — Favourites mirrored to a server collection

User question: can favourites sync with Audiobookshelf, pulling from a
`Favourites` collection if it exists and otherwise creating it when a
favourite is added, and keeping it updated?

- The API documentation site was vague on the paths, so read the server's
  source on GitHub (`server/routers/ApiRouter.js`,
  `controllers/CollectionController.js`, `LibraryController.js`,
  `models/Collection.js`). Endpoints, permissions and the per-library,
  shared nature of collections are recorded in
  [firmware.md](firmware.md#favourites-and-the-server).
- New `main/favourites.c` (the favourites set moved out of `ui.c`): local
  set + pending-change queue in NVS, a sync task, pull on library load,
  create-on-first-add, add/remove.
- Local patch gained `abs_api_call()`; `local.patch` regenerated
  (13 hunks, 4 files).
- Collection replies are expanded and potentially large, so they are
  scanned in one pass instead of parsed with cJSON. Desktop test
  `tools/tests/favourites_scan.sh`: the first version found nothing
  (elements are at depth 3, not 2); fixed, now passes.
- Built (1.83 MB) with the heart animation. Not yet flashed, and the sync
  has never talked to a server.

## 2026-10-05 — Tenth flash

The user put the player in bootloader mode and said to flash. Flashed the
app (1.83 MB) through `/dev/cu.usbserial-130`; hash verified; settings
kept. Carries the heart animation and the favourites collection sync,
which has not yet been run against the server. Serial capture started.

## 2026-10-05 — First favourites sync on the player; race fixed

Tenth build's log (`logs/yotwo_setup_2026-10-05_k.log`):

- `1 favourites on the player, 0 changes waiting to be sent`.
- First collection lookup failed (`-1`, 12 s after power-on, Wi-Fi not yet
  usable); the retry at 49.7 s succeeded: `no "Favourites" collection in
  this library yet`.
- Both-knob presses at 154.3 s and 156.7 s; `created the "Favourites"
  collection` at 160.5 s; a third press at 199.2 s. No crashes.

Reading the code against that timing exposed a race: the second press came
while the create request for the first was in flight. If it was an unmark
of the same book, the queued change was cancelled but the request still
completed with that book in it. Fixed by tracking the change being sent
(`s_sending`) and queuing the opposite change behind it rather than
cancelling. Each add and remove is now logged. Built; not yet flashed.

## 2026-10-05 — Settings submenu and readability mode

User requests: move view and library into a settings submenu at the end of
the menu; add a readability-mode toggle that moves a leading "The" to the
end of a title and reduces "Chapter #" / "Chp #" / "Track #" / "Trk #" to
just the number.

- Menu is now LAST, NEW, FAV, A-Z, AUTH, SERI, CHAP, BED, SET. `SET` opens
  LIB, VIEW, READ; hold returns one level at a time.
- `readable_title()` and `readable_chapter()` in `ui_core.c`, applied to
  book captions, series names, chapter-list captions and the player's
  title and chapter (through a new `get_playback()` wrapper so scroll
  lengths use the reworded text). Also accepts `Chap` and `Ch`, which the
  user did not list but are the same idea.
- Stored in NVS (`ui`/`readable`), off by default.
- Simulator: `docs/media/ui_settings.png`. Built (1.83 MB) together with
  the favourites race fix. Not yet flashed.

## 2026-10-05 — Eleventh flash

The user said to flash. The player was in bootloader mode (esptool
connected). Flashed the app (1.83 MB) through `/dev/cu.usbserial-130`;
hash verified; settings kept. Carries the settings submenu, readability
mode and the favourites race fix with per-change logging. Serial capture
started.

## 2026-10-05 — Eleventh build's log; card design; reader spike

**Log of the eleventh build** (`logs/yotwo_setup_2026-10-05_l.log`): no
crashes. Favourites: the start-up pull found the collection with 0 books
(player also had 0); two books were then added and the server accepted
both. Whether "0 books" was true or a misread is open; recorded in
[firmware.md](firmware.md#hardware-results).

**Card design.** The user switched to plan mode and asked to think through
uses for the NFC reader now that cards are not needed to choose a book.
Discussed: physical bookmark, card = book, game/mode cards, tokens, and
(mine) listener cards, a parent key, routine cards, lucky dip, portable
position. The user's direction: a personal player; build "Yoto mode" (UID →
library item) and routine cards starting with sleep. Answers to three
questions (linking gesture, sleep card removal, timer lengths) went into
the card design (see [firmware.md](firmware.md#cards)). Checked in `player.c` that `player_open()` needs only ID,
title and author, which is what makes cross-library cards possible.

Plan mode was then switched off without the plan being reviewed through
the approval step, so only the uncontroversial first step has been done.

**Step 1: reader spike.** `hardware/nfc.c`: CR95HF on the shared SPI bus
at 2 MHz, wake pulse, reset, echo, ISO 14443-A select, then four polls a
second (WUPA, anticollision, cascade for 7-byte UIDs). Uses the chip's SPI
poll flag instead of the IRQ line on the expander. Logs the first 24 raw
replies and `card <UID> in` / `card out`; the display shows `CARD` when one
arrives. `UI_EV_CARD_REMOVED` became `UI_EV_CARD_IN` / `UI_EV_CARD_OUT`.
If the reader does not answer, the player carries on without it. Built
(1.84 MB). Not flashed.

## 2026-10-05 — Twelfth flash (card reader spike)

The user said the player was in "debug mode" (taken as bootloader mode;
esptool connected) and to flash. Flashed the app (1.84 MB) through
`/dev/cu.usbserial-130`; hash verified; settings kept. First build with
`hardware/nfc.c`. Serial capture started.

## 2026-10-05 — Reader proven; card features built; crackling

User on the twelfth build: `CARD` shows on inserting a card; both knobs
still favourites (expected: linking was not built yet); and there is now
crackling.

Log (`logs/yotwo_setup_2026-10-05_m.log`):
- Reader: echo `0E 55`, protocol `0E 00`, idle replies `0E 87 00`, and
  `card 045D9FFA972091 in` / `card out` five times, matching what the user
  did. This settles the reply layout (flags, code, length, data).
- Favourites: start-up pull `2 books … 2 unchanged`; one add and one
  remove confirmed. The earlier "0 books" was real.
- Audio statistics clean, so the crackling is not underruns or decode
  errors. Suspect the reader's field, on continuously in that build.

Work:
- `nfc.c`: field switched on only around each look (protocol select on,
  6 ms, WUPA/anticollision, protocol select off); looks every 500 ms; gone
  after 2 misses; raw logging cut to the first 6 exchanges.
- `cards.c`: the card store (`cards.json` on SD, cJSON).
- `ui_host.h` / `ui.c`: card host calls; cross-library play through
  `player_open()` with a minimal book; deferred start until Wi-Fi is up;
  `host_current_book()` now returns -1 when the open book is not in the
  loaded library (so a card's book from elsewhere is not mistaken for the
  most recent local one).
- `ui_core.c`: `card_in()` / `card_out()`, sleep mode with freeze, resume
  and extend, link-on-both-knobs for unknown cards, `SET` → `CARD`
  (`SLP` with minutes, `CLR`) and `EXT`. Card events that arrive before
  the library has loaded are held.
- Simulator: `card:<id>`, `nocard`, `mN` (minutes). Walked through book
  card in/out/in, linking a new card, sleep card in/out/in with the clock
  frozen (showed `20M` after 10 of 30), extension after expiry (`5M`),
  making a sleep card from settings.
- One compile error (a declaration used before its definition in `ui.c`).
- Built (1.84 MB). Not yet flashed.

## 2026-10-05 — Thirteenth flash (card features)

The previous serial capture was stopped by its time limit; the rest of its
log held nothing new. The user said to flash. Flashed the app (1.84 MB)
through `/dev/cu.usbserial-130`; hash verified; settings kept. First build
with linking, book cards, sleep cards, the `CARD` and `EXT` settings and
the pulsed reader field. Serial capture started.

## 2026-10-05 — Chapter item removed; scroll speed; reader robustness

User: the chapter item is not needed in the menu (chapters can be scrolled
from the book), and asked whether text scroll speed can be changed. It
could not; that had never been built. Added.

- Menu: `CHAP` removed, with its list screen. Now LAST, NEW, FAV, A-Z,
  AUTH, SERI, BED, SET.
- **Scroll speed** (`SET` → `SPD`): SLOW 8, MED 12 (as before), FAST 18,
  MAX 25 pixels a second, stored in NVS. To keep the fastest a pixel at a
  time the UI tick went from 80 ms to 40 ms; every frame-count constant in
  `ui_core.c` is now written in milliseconds (`MS()`), and scrolling goes
  through `scroll_px()` / `scroll_ticks()`. The display is therefore
  redrawn up to 25 times a second while something is moving (unchanged
  parts are still not resent).
- Simulator: `docs/media/ui_speed.png`.

Thirteenth build's log so far: linking and the book card work on the
player; two false "card out / in" pairs half a second apart; one watchdog
warning during start-up; audio clean. Reader made more tolerant (three
WUPA tries, 10 ms power-up wait, three misses before "gone").

Built (1.84 MB). Not yet flashed.

## 2026-10-05 — Fourteenth flash

The user said to flash. Stopped the running serial capture to free the
port (its log had already been read). Flashed the app (1.84 MB) through
`/dev/cu.usbserial-130`; hash verified; settings kept. Carries the menu
without `CHAP`, the `SPD` setting with the 40 ms tick, and the more
tolerant card presence check. Serial capture restarted.

## 2026-10-05 — Slower speed option; press-to-confirm chapter screen

User requests: drop the fastest scroll speed and add a slower one; and
turning through chapter numbers must only change chapter when the knob is
pressed, with a timeout that backs out if it is not, showing the chapter
number on top and the chapter name scrolling along the bottom (when there
is one that is not just a "Chapter #" variation), the timeout being long
enough for it to scroll.

- Speeds are now MIN 5, SLOW 8, MED 12, FAST 18 pixels a second. A stored
  25 is read as 18.
- The auto-jumping chapter overlay is gone. Turning the right knob on the
  player opens a chapter screen (`M_CHAPTER`): number on top,
  `chapter_name()` along the bottom, position line; press goes there,
  hold or the timeout (3 s plus the name's scroll time) goes back via
  `settle_player()`, which returns to the settled screen rather than
  replaying the title scroll. It wraps round, since nothing changes without
  a press.
- Simulator: `docs/media/ui_chapter_pick.png` (a named chapter, the
  timeout, a confirmed jump, and a book whose chapters are only numbers).
- Built. Not yet flashed.

## 2026-10-05 — Fourteenth build's log; background tasks moved to core 1

Log (`logs/yotwo_setup_2026-10-05_o.log`): card present at power-on
started its book as designed; no false removals; audio clean (slowest
write up from 88 ms to 132 ms with the 40 ms UI tick); two task-watchdog
warnings at start-up naming `favsync` and `cover16` on core 0.

The watchdog warnings keep coming back in different tasks, which says core
0 is simply overloaded during start-up rather than any one task being at
fault. Moved the cover loader and favourites sync to core 1 at priority 1
(below the UI, input, reader and decoder). Built; not yet flashed. This is
in the same pending build as the chapter screen and the new speed options.

## 2026-10-05 — Crackling confirmed gone; start-up animation

User: **the crackling is gone** (since the reader's field was pulsed).
Recorded in firmware.md, hardware.md and the roadmap.

User request: a start-up animation, to give the background work time to
get going, modelled on a CSS loader they supplied
(`~/Downloads/css-loader-91.html`): six coloured bars 100 ms apart, each
rotating about its bottom edge over 2 s, so its visible height swings
from +full to −full.

- `draw_boot_bars()` in `ui_core.c`: bar height = amplitude × cos of the
  bar's phase, drawn up from the middle when positive and down when
  negative; the loader's six colours; bars two pixels wide at
  x = 0, 3, 6, 8, 11, 14 (six bars with gaps do not fit sixteen pixels
  evenly, so the middle two touch).
- Shown during start-up for at least 3 s (`BOOT_MIN`); books that arrive
  sooner wait. After 12 s it becomes a smaller version over the status
  word, so a stuck start still says why.
- Simulator: `docs/media/ui_boot.png`. Built. Not yet flashed; the pending
  build also has the chapter screen, new speeds and the core-1 change.

## 2026-10-05 — Eight bars

User: use eight bars so there are no gaps. The animation now has eight
two-pixel bars filling all sixteen columns; teal (`#1ABC9C`) and pink
(`#E84393`) were added to the loader's six colours, my choice. Same 100 ms
step between bars and 2 s cycle. Simulator image regenerated. Built; not
yet flashed.

## 2026-10-05 — Fifteenth flash

The user put the player in bootloader mode and said to flash. Flashed the
app (1.85 MB) through `/dev/cu.usbserial-130`; hash verified; settings
kept. Carries the eight-bar start-up animation, the press-to-confirm
chapter screen, the MIN/SLOW/MED/FAST speeds, and the cover loader and
favourites sync on core 1. Serial capture started.

## 2026-10-05 — Fifteenth build's log: watchdog quiet, memory low

The capture stopped with "Device not configured": the serial adapter was
unplugged about five minutes in. The log file contains non-text bytes at
the end (the brown-out message), which made plain `grep` print nothing;
re-read with the bytes stripped.

- No watchdog warnings: the core-1 move worked. Covers took 63 s to load.
- Audio clean. Library switch to Podcasts worked. Favourites: 3 books.
- Power button held → `ui: power off` → `BOD` (brown-out detector): the
  latch release cuts power on battery. First confirmation that power-off
  works.
- **Internal RAM ran down to 11.7 KB** during the library switch and sat at
  22 KB afterwards. Changed `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` from
  4096 to 256 so mid-sized allocations go to PSRAM. Built; not flashed.
  Untested, and worth watching in the next logs (`int` in the player's
  statistics line).

## 2026-10-06 — Battery display

The user reset the player and asked for the log, but no capture was
running (the last one died when the adapter was unplugged), so that
start-up was not recorded. The adapter was back; a 25 s listen heard
nothing (the player logs nothing while idle), and a new background capture
was started.

User request: show the battery for a moment when the power button is
pressed and when the power cable is unplugged, with a full-screen battery
icon showing level and charging; and look at how the stock firmware did it.

The original firmware shows an icon for 3 s on power-state changes and a UI event; the V2 table
has five level icons on battery and one "powered" icon; the charger status
lines are read inverted, i.e. active low.

Ours:
- `battery.c`: 8-sample average, finer percentage curve (my estimate),
  plugged / charging from expander 1.5 and 1.7, a once-a-minute log line
  with the raw pin levels.
- `input.c`: `UI_EV_POWER_PRESS` on a short press; `UI_EV_PLUGGED` /
  `UI_EV_UNPLUGGED` from the plug line.
- `ui_core.c`: `OV_BATTERY` overlay, 3 s, `draw_battery()`.
- Simulator: `pwr`, `plug`, `full`, `unplug`, `bat:N`;
  `docs/media/ui_battery.png`.
- Built (1.85 MB), together with the pending internal-memory setting. Not
  flashed.

## 2026-10-06 — Sixteenth flash

The earlier background capture ended having recorded nothing (the player
was idle or off for the 25 minutes it listened). The user put the player
in bootloader mode and said to flash. Flashed bootloader, partition table
and app (1.85 MB) through `/dev/cu.usbserial-130`; hashes verified;
settings kept. Carries the battery display, the charger status reading
and the internal-memory setting (`SPIRAM_MALLOC_ALWAYSINTERNAL=256`).
Serial capture started before the reset.

## 2026-10-06 — Sixteenth build's log; battery redesign; signing in again

Log `logs/yotwo_2026-10-06_b.log` (git-ignored):

- **Signed out.** From the first request of this start-up every API call
  got 401, and `/auth/refresh` got 401 too. The last successful renewal
  was in the previous day's session; no settings-save error in any log.
  Cause not established. Two candidates: the server issued a new refresh
  token that the player did not keep, or the session was ended on the
  server. Nothing could be played, so the internal-memory figure for the
  new `SPIRAM_MALLOC_ALWAYSINTERNAL` setting is still unknown.
- **Battery.** Power press read 3933 mV, 77 %, both status lines high
  (on battery). One `plugged in` at 126 s, both lines low (charging),
  smoothed voltage 4150 mV, which means the raw reading was about 4.37 V:
  the charger's voltage. No `unplugged` event in the remaining five
  minutes, and the user saw no battery display on plugging in or out.
  Not explained.

Changes (user's request, plus what the log called for):

- `draw_battery()` redrawn: outline rows 5-10, level rows 6-9, red < 20,
  orange < 40, green above, no number, white bolt when on external power.
  `ui_battery_t` now carries `plugged` instead of charging / charged.
- Battery events are handled before the status / setup screens return
  early, so the display appears on any screen, and a scrolling message no
  longer replaces it.
- `battery.c`: level held at the last on-battery value while charging.
- `input.c`: logs each change of the plug and charge lines.
- `SET` → `SIGN`: starts the setup portal from the player
  (`host_setup_start()` / `host_setup_stop()`); the main loop shows
  "SIGNED OUT - SET, SIGN TO SIGN IN AGAIN" once when the server refuses
  the sign-in.
- `local.patch`: the renewal now logs whether the server returned a new
  refresh token, the same one or none, and on failure the token length
  and the start of the server's reply. Never the tokens themselves.
- Simulator strip `docs/media/ui_battery.png` regenerated. Built
  (1.85 MB). Not flashed.

## 2026-10-06 — Upright battery; why the player was signed out

- User: make the battery vertical so the bolt fits. `draw_battery()` now
  draws it upright: six wide, four columns of level, ten rows, terminal
  on top, a slimmer seven-row bolt. Strip regenerated.
- Sign-out, traced as far as the logs allow. Session n (22:20 on the
  5th) renewed successfully; o and p (to 23:06) made no renewal, their
  access token still being fresh; the next captured session (09:28 on the
  6th) was refused from its first renewal. The server's source shows the
  refresh token is replaced at every renewal with a ten-minute grace for
  the old one. Ruled out: a stale copy of the settings saved over the new
  tokens (the only whole-settings save is the setup page); a settings-save
  error (none logged). Not ruled out, and not provable from what was
  captured: a renewal in an uncaptured session between 23:06 and 09:28
  whose reply was lost or could not be parsed, or the session being ended
  on the server. The exact cause is therefore **unknown**.
- Hardening against the lost-reply case, which is real whatever happened
  here: tokens scanned from the reply text rather than parsed with cJSON;
  immediate retries; more logging. See "Staying signed in" in
  `docs/firmware.md`. An API key avoids the mechanism altogether.
- Built (1.85 MB). Not flashed.

## 2026-10-06 — Seventeenth flash

The user put the player in bootloader mode and said to flash. Flashed
bootloader, partition table and app (1.85 MB, commit 2a1c39a) through
`/dev/cu.usbserial-130`; hashes verified; settings kept. Carries the
upright battery display, `SET` → `SIGN`, the signed-out notice, the
plug-line logging and the hardened token renewal. Serial capture
`logs/yotwo_2026-10-06_c.log` (git-ignored) started before the reset.

## 2026-10-06 — Seventeenth build's first log

`logs/yotwo_2026-10-06_c.log`: the player started and ran for 29 minutes
with no knob, button or cable events. Findings:

- The server's reason for refusing the renewal is `Invalid refresh
  token`: in the server's code, no session holds that token, or it is a
  replaced token past its ten-minute grace. Consistent with a renewal
  whose new token the player never kept; it does not say when.
- The signed-out notice was raised once (`main: signed out by the server`).
- At start-up both status lines read low (`plug pin 0, charge pin 0`).
  Whether the cable was in is to be asked.
- The player asked the server to renew every 30 s for the whole session.
  Changed: once refused, it does not ask again until signed in afresh
  (the server limits sign-in attempts). Built, not flashed.

## 2026-10-06 — Own setup page; loading wheel; resume on left press

User's requests after trying `SET` → `SIGN`:

- The phone remembered the setup network with its old password, so it
  had to be forgotten each time. Asked for an open network with the
  password on the page instead. Done: open `Yotwo-XXXX`, a 4-digit code
  on the display, entered on the page before anything is shown or
  accepted (`wifi.c` patched to start an open access point when given no
  password).
- ABSpresso's page had settings that mean nothing here and lacked this
  player's. New `firmware/main/setup/` replaces it; see "Setup page" in
  `docs/firmware.md`. The page's script passes `node --check` and the
  page was rendered in headless Chrome; the handlers are untested until
  flashed.
- `LOAD` under the wheel removed while a book loads: the wheel alone,
  centred.
- Left press with nothing open (just switched on) now starts the
  unfinished book played most recently (`host_current_book()`), as the
  sleep card already did.
- Simulator strip `docs/media/ui_load_setup.png`. Built (1.85 MB). Not
  flashed.

## 2026-10-06 — Eighteenth flash

The user put the player in bootloader mode and said to flash. Flashed
bootloader, partition table and app (1.85 MB, commit 4e39273) through
`/dev/cu.usbserial-130`; hashes verified; settings kept. Carries the own
setup page on an open network, the loading wheel, resume on left press,
and the stop on repeated renewals. Serial capture
`logs/yotwo_2026-10-06_e.log` (git-ignored) started before the reset.

## 2026-10-06 — Eighteenth build's log

`logs/yotwo_2026-10-06_e.log`, about ten minutes:

- **Own setup page ran on the player.** `SET` → `SIGN` started the open
  network `Yotwo-FEE5`; a phone joined 8 s after it came up and the code
  was accepted 11 s later. The user then left with a hold; nothing was
  saved, so the save path of the new portal is still untested.
- **Plug events now arrive both ways.** Unplug at 160 s (plug line 0 → 1,
  charge line already 1 a moment before), plug-in at 194 s (plug 1 → 0,
  then charge 1 → 0). The battery display was triggered each time, and by
  two power-button presses. The polarity reading is confirmed. Level
  reported: 100 % each time.
- Left press with nothing open started the last book (play at 142 s). A
  book card played its book at 207 s.
- Internal memory 61–69 KB free throughout; no refusals from the server;
  no errors other than the two start-up pull-up notices for the
  input-only pins.

## 2026-10-06 — Clock

User asked for a clock: time in the bottom half, and in the top half a
sunrise and sunset (two-tone orange sun rising from the halfway line,
grey full moon fading, background from dark to warm yellow), with
sunrise time, sunset time and transition time configurable, 12-hour by
default. Also, for later, a wake-up light on the LED ring.

- `ui_core.c`: `draw_clock()`, `daylight()`, idle logic (`s_clock_on`),
  `SET` → `CLK` with `HOUR` / `RISE` / `DUSK` / `FADE`.
- `ui_host.h`: `host_time()`, `host_clock_setting()` and setter,
  `host_timezone()` and setter; implemented in `ui.c` (NVS) and the
  simulator.
- `main.c`: SNTP started after Wi-Fi.
- Setup page: a Clock section (time zone, sunrise, sunset, fade, 12/24).
- Decisions of mine, to confirm: the clock appears after 30 s idle with
  nothing playing; default time zone UK; no am/pm mark.
- Simulator strips `docs/media/ui_clock.png`, `ui_clock_settings.png`.
  Built (1.86 MB). Not flashed. Wake-up light added to the roadmap.

## 2026-10-06 — Nineteenth flash

The user put the player in bootloader mode and said to flash. Flashed
bootloader, partition table and app (1.86 MB, commit 224d5aa) through
`/dev/cu.usbserial-130`; hashes verified; settings kept. Carries the
clock, its settings and the time sync. Serial capture
`logs/yotwo_2026-10-06_f.log` (git-ignored) started before the reset.

## 2026-10-06 — Clock time choices: AM/PM and fixed layout

User, after trying the nineteenth build: choosing a time gave no AM/PM,
and the figures shifted when the hour reached two digits. `RISE` and
`DUSK` now use `draw_time_choice()`: hours left of a fixed colon, minutes
right, `AM` / `PM` above (clock icon in 24-hour mode). Strip
`docs/media/ui_clock_settings.png` regenerated. The log of that build's
session held only a clean start-up; a "time known" log line was added.
Built, not flashed.

## 2026-10-06 — Offline copies, stage 1

Asked how the stock player handles downloads and how ours should. The
user's decisions: automatic for cards and favourites; charger only;
minimise simultaneous transcodes, with (1) a background queue of whole
books when paused, on the charger and on Wi-Fi, and (2) later, a
play-through cache.

- New `offline.c`: the queue and its conditions; removes copies no longer
  wanted. `download.c` patched with `download_set_paused()`.
- `favourites_ids()`, `cards_books()`, `host_book_offline()` and the blue
  mark in the lists.
- Built (1.86 MB). Not flashed; no download has ever run on this player,
  so this is untested beyond compiling and the simulator's mark.

## 2026-10-06 — Twentieth flash

The user put the player in bootloader mode and said to flash. Flashed
bootloader, partition table and app (1.87 MB, commit 020d16a) through
`/dev/cu.usbserial-130`; hashes verified; settings kept. Carries offline
copies stage 1, the AM/PM time picker and the "time known" log line.
Serial capture `logs/yotwo_2026-10-06_g.log` (git-ignored, up to 110
minutes) started before the reset.

## 2026-10-06 — Twentieth build's log: first download on the player

`logs/yotwo_2026-10-06_g.log`, about 62 minutes, ended when the serial
adapter was disconnected.

- Offline stage 1 runs. The card's book (34 hours, 20,387 segments) was
  taken first and resumed from segment 84 left by an earlier attempt.
- Holds on play and releases on pause within one 5 s check, each time
  resuming from the last committed segment.
- Progress: 10 % at 27 min, 19 % at 41 min, then slower, 22 % at 62 min.
  Why it slowed is not known (the server's transcode speed, or the
  repeated holds, are candidates).
- Occasional TLS read errors and one connect time-out; all retried
  without failing the download.
- Time sync works: `time known: 15:08 local`, 5.5 minutes after start.
  That delay is long and unexplained.
- The user saw no steady bar: expected, as nothing had completed.

## 2026-10-07 — Signing in again by itself

User asked whether the username and password could be stored hashed or
encrypted and used to sign in again when the session ends. Hashing cannot
work (the password itself is needed); encryption on the device is only
as good as the key, which is also on the device. Built anyway as the
practical answer, with that limit stated: `setup/credential.c` (AES-256
under a chip-derived key), automatic sign-in from the renewal failure
path, a tick box on the setup page. Built (1.87 MB). Not
flashed.

## 2026-10-07 — Clock on a hold

User: holding the right knob on the book page should go straight to the
clock. Done for the player screen (`M_PLAYER`), where a hold did nothing
before; the clock then persists through playback (`s_clock_held`).
Checked in the simulator. Built, not flashed.

## 2026-10-07 — Twenty-first flash

The user said to flash. Flashed bootloader, partition table and app
(1.87 MB, commit 083e3e4) through `/dev/cu.usbserial-130`; hashes
verified; settings kept. Carries the kept-password sign-in and the clock
on a right-knob hold. Serial capture `logs/yotwo_2026-10-07_a.log`
(git-ignored, up to 110 minutes) started before the reset.

## 2026-10-07 — Time asked for at once; darker night

Log `logs/yotwo_2026-10-07_a.log`: Wi-Fi had its address at 4.3 s and a
book was playing by 18 s, yet the clock was refused (three right holds,
30 to 57 s) and the time was first available at or before 87 s. Cause:
SNTP was started before Wi-Fi was up, so its first request failed and
lwIP waited 15 s, then 30, then 60 before asking again. Now started from
the got-IP event, restarted on each reconnection, with a log line when
the time arrives.

User also asked for a darker night: the background is now unlit at night
and dims by the square of the daylight during the transitions; the moon
is less than half as bright. Strip `docs/media/ui_clock.png` regenerated.
Built (1.87 MB). Not flashed.

## 2026-10-07 — Brightness by day and night, idle dim, switching off

User asked whether the stock player dims when idle (it has day and night
levels, a display time-out and an automatic shut-down), then for: an idle dim after 60 s; 80 % by day and
40 % by night; and switching off on battery after 60 minutes by default,
settable to 15, 30, 60, 120 or never. Built as described in
`docs/firmware.md`. Simulator: 80 → 20 idle → 80 on a touch → 40 at
night → 10 idle → power off after an hour on battery; no power-off on
external power. Built, not flashed.

## 2026-10-07 — Twenty-second flash

The user said to flash. Flashed bootloader, partition table and app
(1.87 MB, commit d6a38d5) through `/dev/cu.usbserial-130`; hashes
verified; settings kept. Carries the time asked for on got-IP, the unlit
night sky and dimmer moon, day and night brightness, the idle dim and the
automatic switch-off. Serial capture `logs/yotwo_2026-10-07_b.log`
(git-ignored) started before the reset.

## 2026-10-08 — Review, fixes and tests

User asked for a review of the code and setup, missed features, and
tests. Written up in a review (not published): nine bugs fixed (the
worst: a downloaded book resuming from a stale or zero position and then
overwriting the server's), ten open risks, the stock features ours lacks,
and what the new tests cover. `tools/tests/run_all.sh` runs 95 checks on
the desktop, all passing. The twenty-second build's capture was empty
(the player was not reset while it listened). Built (1.87 MB). Not
flashed.

## 2026-10-08 — Volume limit

User: a volume limit, default 80 %, in steps of 10 from the settings,
with the UI scaled 0 to the limit rather than stopping at it. Done in
`ui.c` (scaling), `ui_core.c` (`SET` → `VOL`), the setup page, and the
tests (4 more checks of the scaling, 2 scenarios). Built, not flashed.

## 2026-10-08 — Twenty-third flash

The user put the player in bootloader mode and said to flash. Flashed
bootloader, partition table and app (1.87 MB, commit 5ae2e27) through
`/dev/cu.usbserial-130`; hashes verified; settings kept. Carries the
nine review fixes and the volume limit. Serial capture
`logs/yotwo_2026-10-08_a.log` (git-ignored) started before the reset.

## 2026-10-08 — Twenty-third build's log

`logs/yotwo_2026-10-08_a.log`, two sessions (a restart after saving setup).

Working:
- Time asked for at 4.2 s (address received), arrived at 9.1 s.
- Settings store: 226 of 756 entries used.
- Card in and out within 1.6 s: the book opened and was paused at once
  (downloads were released 0.2 s after it started).
- Headphone socket is seen, and the speaker follows it.
- Setup page saved for the first time on the player: signed in,
  `password kept for signing in again`, restarted.
- Download of the 34-hour book went from 60 % to 65 %.
- Internal memory 54 to 63 KB free.

Problems:
- **The 34-hour book's position on the server is 0.** Every session for
  it in this log opens `at 0/122321 s`, the first of them (a download
  session, 30 s after start-up) before anything was played. In the last
  log that has it (7 Oct, 38 s in) it was 25759 s. So it changed in the
  unlogged time in between, on the twenty-first or twenty-second build.
  Ruled out by reading the code and the server's source: download
  sessions (closed with an empty body, which the server ignores; and the
  position survived a day of them), and the local-position push (only for
  completed downloads). Not ruled out: the first chapter being chosen on
  the chapter screen, a restart of the book on the server, or a fault not
  yet found. **Cause unknown.** Chapter choices are now logged.
- **Setup ran out of sockets.** `httpd_accept_conn: error in accept (23)`
  three times over, and two sign-in attempts failed with `Failed to
  create socket` before the third worked. The page's server allowed 7
  connections of the system's 10, with a download and the favourites
  sync also running. Now: 4 for the page, 16 in all, and downloads are
  held while setup is open.
- The code was asked for three times in one setup session: the phone
  reloads the page and the page forgot it had been unlocked. It now
  remembers for as long as the tab lives.
- The headphone socket chatters (in/out/in/out within 60 ms). Now
  debounced like the cable.

Built (1.87 MB). Not flashed.

## 2026-10-08 — The player's web page

User asked for a local server at `yotwo.local`: all the settings, what
is playing, what is downloaded and its progress, removing downloads, a
look at the SD card, and a name for the player that makes the address
(`toms-yotwo.local`). Built as `web/web.c` + `web_page.h`, with
`setup/player_settings.c` shared with the setup page, a skip list in
`offline.c`, `download_list()` in `local.patch`, and the `espressif/mdns`
component. See "The player's web page" in `docs/firmware.md`. Wi-Fi and
sign-in are deliberately left to `SET` → `SIGN`. App is now 1.95 MB.
Tests: 19 new checks (`web_units.sh`), page checks extended to both
pages. Not flashed.

## 2026-10-08 — Web page: covers and request checks

User asked for artwork on the page and sensible security (CORS rules):
easy to open on a phone, but safe against bare posts and the like.

- Covers: the 64-pixel JPEG is now kept on the card when fetched
  (`abs/c64/`), with a background catch-up for covers cached earlier;
  `GET /api/cover` serves it, or the 16 x 16 picture as a BMP. Only the
  16 x 16 version existed on the card before this.
- Checks on every request (Host, Origin, Sec-Fetch-Site, a required
  header, JSON for changes, preflights refused) and hardening headers;
  see "The player's web page" in `docs/firmware.md`.
- Tests: `web_units.sh` now has 44 checks, 25 of them requests that must
  be let in or refused. The page was run again in a headless browser with
  stand-in covers: no script errors, every data request carried the
  header.
- Built (1.95 MB). Not flashed; the server has still never run on the
  player.

## 2026-10-08 — Twenty-fourth flash

The user put the player in bootloader mode and said to flash. Flashed
bootloader, partition table and app (1.95 MB, commit 15e31da) through
`/dev/cu.usbserial-130`; hashes verified; settings kept. Carries the
player's web page (name, mDNS, status, settings, downloads, files,
covers, request checks) and the fixes from the twenty-third build's log
(setup sockets, setup code remembered, headphone debounce). Serial
capture `logs/yotwo_2026-10-08_b.log` (git-ignored) started before the
reset.

## 2026-10-08 — Download speed: what limits it

User asked whether downloads can be sped up, assuming the server's
transcoding is the limit. Findings:

- The server is probably **not** the limit. Its `Stream.js` uses
  `-c:a copy` (no re-encoding) unless the source is a format that has to
  become AAC (`codecsToForceAAC`), and does not pace ffmpeg to real
  time. For an ordinary m4b or mp3 it only cuts the audio into segments.
- Measured rate, 8 Oct: 60 % to 71 % of 20,387 segments in 1,785 s, about
  1.3 segments a second (7.5 x real time). On 6 Oct it was about 2 a
  second.
- Each 6-second segment is its own HTTPS request, and the radio was in
  modem-sleep (`WIFI_PS_MIN_MODEM`), which delays the answer to every
  request by up to a beacon interval.

Changed (in `local.patch`, `download.c`): Wi-Fi power saving is off
while a download runs (they only run on external power) and restored
after; the read/write block is 8 KB instead of 4; and once a minute the
downloader logs its speed and how its time divides between waiting for
the server, receiving and writing to the card. That line will show
where the limit really is. Built, not flashed.

Also seen in the twenty-fourth build's log so far: the web page came up
and was used (three copies removed from it, including the part-fetched
34-hour book).

## 2026-10-08 — Web page: cards

User asked for the linked cards on the web page, with a way to unlink
them. Added a Cards view (`/api/cards`), `cards_list()`, and a mutex in
`cards.c` now that the web server's task removes records. Rendered in a
headless browser with sample data; tests pass. Built (1.96 MB). Not
flashed.

## 2026-10-08 — Twenty-fifth flash

The user put the player in bootloader mode and said to flash. Flashed
bootloader, partition table and app (1.96 MB, commit 0a1eaaf) through
`/dev/cu.usbserial-130`; hashes verified; settings kept. Carries the web
page's covers, request checks and Cards view, and the download speed
changes with their once-a-minute measurement. Serial capture
`logs/yotwo_2026-10-08_c.log` (git-ignored) started before the reset.

## 2026-10-08 — Twenty-fourth build's log: web page in use, and a crash

`logs/yotwo_2026-10-08_b.log`:

- The web page ran on the player for the first time: up 27 s after
  start (it waited behind the library load), loaded from a phone, three
  copies removed from it. No request was refused. The phone also asked
  for `/favicon.ico` and touch icons (405, harmless).
- **One crash**, 4.4 s after the first start on that build:
  `LoadProhibited` on core 1, then a clean restart. Decoded by rebuilding
  commit 15e31da (same size, so same addresses):
  `cover_task` → `load` → `have_jpeg` → `stat` → FatFs → `sdmmc_read_sectors`
  → `sdspi_host_start_command` (sdspi_host.c:469). The instruction that
  faulted had just reloaded the `cmd` pointer from the stack slot it was
  saved to a few instructions earlier and got `0x80177e19`, a return
  address, instead. The cover task's stack was in PSRAM. A stored value
  reading back wrong is the fault ESP-IDF documents for this chip
  revision ("a write to PSRAM not to take place ... when the cache line
  needs to be fetched from external RAM and an interrupt occurs"). The
  compiler workaround is on; this would be a case it does not cover.
  **That reading fits the evidence but is not proven**, from one event.
- Mitigation: the cover task's stack moved to internal memory (8 KB).
  The other tasks with PSRAM stacks (downloader, offline, favourites,
  both web servers) are unchanged, so the same thing could in principle
  happen in them; none has been seen to. The web page now starts before
  the library load. Built, not flashed.

## 2026-10-08 — Twenty-sixth flash

The user put the player in bootloader mode and said to flash. Flashed
bootloader, partition table and app (1.96 MB, commit c7c6184) through
`/dev/cu.usbserial-130`; hashes verified; settings kept. Carries the
cover task's stack in internal memory and the web page starting before
the library load, on top of the twenty-fifth build (Cards view, download
speed changes), which ran for only about ten seconds (it started normally, got its
address and the time, and was then put back into the bootloader). Serial capture `logs/yotwo_2026-10-08_d.log` (git-ignored)
started before the reset.

## 2026-10-08 — Twenty-sixth build's log: second crash, download speed measured

`logs/yotwo_2026-10-08_d.log`, about 30 minutes plus a restart.

- **Crashed again**, 9 to 11 s after start, then ran normally after the
  restart. Decoded against this build: `httpd` task → `on_downloads` →
  `send_downloads` → `stat` → FatFs → `sdmmc_read_sectors` →
  `start_command_read_blocks` (sdspi_host.c:725), `LoadStoreError`
  through a pointer holding a code address. Same shape as the first:
  an SD card call from a task whose stack is in PSRAM, a value on that
  stack reading back as something else. So moving the cover task was not
  the cure; the web server's task did the same.
- What the two have in common, and what tasks that have never crashed
  lack, is not established. The downloader has made SD calls from a
  PSRAM stack for hours without this. Both crashes were a task's first
  SD call, seconds after start-up, with much else going on.
- Change: SD card calls no longer run on PSRAM stacks at all.
  `storage_call()` (storage.c) runs a function on a helper task with a
  5 KB internal stack; `storage_read_file`, `storage_write_file`,
  `storage_mkdirs` and the new `storage_file_size` use it themselves;
  the web server's folder listing and file reading go through it; the
  downloader's own task has an internal stack (local.patch). The cover
  task is back on a PSRAM stack, its card calls being routed. Remaining
  PSRAM stacks (cover, favourites, offline, web servers, progress sync)
  no longer touch the card directly.
- **Download speed, measured** on a 41-minute book: 110 KB/s, 3.4 x real
  time, steady. Time: 64 % writing to the card, 24 % receiving, 11 %
  waiting for the server. So the limit is writing to the SD card (wired
  for SPI on this board), not the server and not the network. The
  earlier 34-hour book's 8 to 12 x real time is the same byte rate at a
  lower bit rate. The book completed (409 segments in ten minutes).
- Web page: up 0.1 s after the address was assigned; a card was
  unlinked from it. Internal memory free after the library load: 35 KB
  (with the cover task's stack internal, as flashed).
- Built (1.96 MB). Not flashed.

## 2026-10-08 — Downloads: receiving and writing side by side

User asked for receiving and writing at the same time (when idle and on
the charger, which is the only time downloads run). `download.c` now has
a receiver task filling three segment buffers in PSRAM and the download
task writing them out; see "Offline copies" in `docs/firmware.md`. The
speed log reports the two sides separately. Expected gain: up to about
half as much again, since writing was 64 % of the time. Built (1.96 MB),
compiles and the desktop tests pass; the pipeline itself is untested
until flashed.

## 2026-10-08 — Twenty-seventh flash

The user put the player in bootloader mode and said to flash. Flashed
bootloader, partition table and app (1.96 MB, commit 604684f) through
`/dev/cu.usbserial-130`; hashes verified; settings kept. Carries SD card
calls kept off PSRAM stacks (after the two start-up crashes) and
downloads with receiving and writing side by side. Serial capture
`logs/yotwo_2026-10-08_e.log` (git-ignored) started before the reset.

## 2026-10-08 — Download mark: a filling square

User asked for the download mark to be a 3 x 3 blue square at the top
left, filling from the bottom left like the battery, grey-blue while
empty, the next pixel flashing only while a download is running, and
gone when finished. Done (`draw_offline()`); `host_book_offline()` now
gives the percentage and whether it is being fetched. Nine scenario
tests added. Built, not flashed.

## 2026-10-08 — Twenty-seventh build's log; twenty-eighth flash

`logs/yotwo_2026-10-08_e.log`, about 27 minutes, one start:

- **No crash at start-up** (one start only, so not yet proof).
- **Downloads side by side work.** A 2-hour book (1,267 segments) came
  down complete in 15 minutes at 173 to 181 KB/s, 8.2 to 8.6 x real
  time, against 110 KB/s before. The writer is now busy 99 % of the
  time and the receiver is held up by it for 28 %: the SD card is the
  whole limit and there is nothing left to gain on the network side.
- A complete copy was then played from the card. The request for the
  server's position failed once (connection reset), so the position from
  the library data was used, as designed.
- Internal memory free after the library load: 30 KB (it was 35 KB on
  the last build and about 60 KB before the web page, mDNS and the
  internal stacks). Lower than is comfortable.
- Two TLS read errors during the download; both retried.

Twenty-eighth flash: the user put the player in bootloader mode and said
to flash. Bootloader, partition table and app (1.96 MB, commit 60eeb04);
hashes verified; settings kept. Adds the filling-square download mark.
Capture `logs/yotwo_2026-10-08_f.log` started before the reset.

## 2026-10-08 — Offline mode and the Downloaded list

User asked for an offline mode: a `Downloaded` list (complete copies
only), an automatic switch to showing only downloaded books when there
is no Wi-Fi after several tries, and everything back with progress
synced on reconnecting. Built: `V_DL` / `DOWN`, `s_offline` in the UI
core, `host_offline()` (20 s without Wi-Fi) and `host_book_downloaded()`,
resume-on-left-press restricted to downloaded books offline, and an
immediate library reload in `main.c` when Wi-Fi returns. See "Offline
mode and the Downloaded list" in `docs/firmware.md`. 14 scenario tests;
suite passes. Built (1.96 MB). Not flashed.

Also: the twenty-eighth build's first capture timed out before the
player was reset; a second capture caught it from 17 s after a reset:
running normally, internal memory 44 KB free after the library load.

## 2026-10-08 — Wi-Fi symbols for going offline and coming back

User asked for a Wi-Fi symbol with a red cross, and a plain Wi-Fi symbol,
in place of the words OFFL and WIFI. `draw_wifi()`, overlay `OV_WIFI`,
2.5 s. Tests updated (66 scenarios). Built, not flashed.

## 2026-10-08 — Twenty-ninth flash

The user put the player in bootloader mode and said to flash. Flashed
bootloader, partition table and app (1.96 MB, commit c1225ab) through
`/dev/cu.usbserial-130`; hashes verified; settings kept. Carries offline
mode, the Downloaded list and the Wi-Fi symbols. Serial capture
`logs/yotwo_2026-10-08_h.log` (git-ignored) started before the reset,
this time waiting up to 100 minutes for it.

## 2026-10-08 — Twenty-eighth build's log (second capture)

`logs/yotwo_2026-10-08_g.log`, from 17 s after a reset to about 36
minutes:

- A 2-hour book downloaded complete in 16 minutes at 157 to 175 KB/s,
  the writer busy 99 % of the time: as the day's earlier measurement.
- Four **task-watchdog warnings** during it (`IDLE0` on core 0 not run
  for 15 s; `download` running there). Warnings only: nothing restarted.
  The writer never yields, since the SD driver polls. Changed: one tick
  of rest after each segment. Built, not flashed.
- No crash; internal memory 44 KB free after the library load.

## 2026-10-08 — Downloads deleted and fetched again; a cover that will not show

User: some covers do not load on the web page ("Diary of a Wimpy Kid"),
and books already downloaded appear to be downloading again.

**Downloads.** `logs/yotwo_2026-10-08_h.log`, 22.9 s after start: a TLS
read error, then `"Favourites" collection: 0 books (... 3 no longer
favourites ...)`; 5 s later the three complete copies were removed; 17 s
after that the collection read correctly (3 books) and the first of them
began downloading again from nothing. The same in miniature in the `e`
log (1 of 2 dropped, then back). Chain of causes:

1. `request_once()` (ABSpresso's `abs_api.c`) returned status 200 with
   whatever had arrived when the connection failed mid-reply. The
   favourites scanner found no books in the fragment.
2. The favourites then took the server at its word and dropped all
   three.
3. `offline.c` removed the copies of books no longer wanted at its next
   5-second pass.
4. A second library reload ran straight after the first at start-up
   ("Wi-Fi is back", wrongly: it had never been away), doubling the
   chances of 1.

Fixes: a reply cut short is discarded and treated as a failure
(`local.patch`); a copy is removed only after its book has stayed
unwanted for two minutes; "Wi-Fi is back" needs Wi-Fi to have been lost
first. About 5 hours of downloaded audio had to be fetched again
because of this.

**Covers.** The web page shows the JPEG kept on the card, else the
16 x 16 picture. A book whose cover the player could not decode has
neither (its 16 x 16 file is a two-byte "none" marker, and the JPEG was
only kept for covers fetched since yesterday), so the page gets a 404,
seen in the logs. Whether that is the case for this particular book is
an inference: the logs do not name it. Changed: such covers now have
their picture fetched and kept for the web page all the same, served as
whatever format it really is, and a decode failure logs the first bytes
of the file so the reason can be found. Also, covers processed in the
first seconds after start-up missed their JPEG catch-up if Wi-Fi was
not yet up; they get it on a later start.

Built (1.96 MB). Not flashed.

## 2026-10-08 — Thirtieth flash

The user put the player in bootloader mode and said to flash. Flashed
bootloader, partition table and app (1.96 MB, commit 51cdb42) through
`/dev/cu.usbserial-130`; hashes verified; settings kept. Carries the
discarding of cut-short replies, the two-minute grace before copies are
removed, the cover changes, and the downloader's rest between segments.
Serial capture `logs/yotwo_2026-10-08_i.log` (git-ignored) started
before the reset.

## 2026-10-08 — Thirtieth build's log: no SD card; covers that will not decode

`logs/yotwo_2026-10-08_i.log`:

- **The SD card did not mount**: `sdmmc_card_init failed (0x107)`
  (time-out) 2.6 s after start, so no cache, no cards, no downloads and
  nothing under Files for that session. Nothing in the storage start-up
  changed in this build, and the card mounted in the three before it.
  The previous session was ended (into the bootloader) while a download
  was writing to the card, and a reset does not remove the card's power.
  Most likely the card was left mid-write and did not answer the first
  time. **Not proven.** Changed: up to five attempts to mount, 0.4 s
  apart. A full power-off (cable out, power button held) should bring it
  back on the build that is flashed.
- **71 of 159 covers in this library cannot be decoded** by the player:
  `esp_jpeg` error 8 (`JDR_FMT3`, an unsupported kind of JPEG) from the
  bundled decoder (the ROM one is not in use: `CONFIG_JD_USE_ROM` is
  off and `tjpgd.c` is built). With no card to read the 16 x 16 cache
  from, every cover was fetched afresh, which is how this came to light;
  the 6 October log shows the same for this library (57 then). So these
  covers have been showing the plain icon on the player, and nothing on
  the web page, all along. TJpgDec accepts only baseline JPEG with
  4:4:4, 4:2:0 or 4:2:2 colour sampling; these are presumably another
  sampling or progressive. To be confirmed by looking at one of the
  files, which the card will hold once it is back.
- The web page was up 0.1 s after the address; internal memory 51 KB.

Built (1.96 MB). Not flashed.

## 2026-10-08 — Updating over Wi-Fi

User asked to revisit over-the-air updates: upload through the web page,
or point the player at a GitHub release. Built the first: two-slot flash
layout, `web/ota.c` (write, check, set to start, trial and fallback),
upload endpoints guarded by a code shown on the display, and a Firmware
section on the page. See "Updating over Wi-Fi" in `docs/firmware.md`.
GitHub releases left for a decision (public repository or a token).

The change of layout needs one flash by cable that also writes a blank
`otadata` at `0x790000`; every later cable flash must include that file
too. App 1.97 MB of a 3.75 MB slot. Tests: 4 more request checks; the
page's Firmware section rendered with no script errors. Nothing of the
update path itself can be tested before it is on the player.

## 2026-10-08 — Thirty-first flash: the two-slot layout

The user put the player in bootloader mode and said to flash, having
been told this one changes the flash layout. Flashed through
`/dev/cu.usbserial-130`, all hashes verified:

| Offset | File |
|---|---|
| `0x1000` | `bootloader.bin` (now with rollback support) |
| `0x8000` | `partition-table.bin` (nvs, phy_init, ota_0, ota_1, otadata, coredump) |
| `0x790000` | `ota_data_initial.bin` (blank: start `ota_0`) |
| `0x10000` | `yotwo.bin` (1.98 MB, commit 5470d3c) |

Settings (`nvs` at `0x9000`) were not touched. **From now on every cable
flash uses these four**, not the earlier three. Carries over-the-air
updates, the SD card retries, and the fixes for cut-short replies and
hasty copy removal. Serial capture `logs/yotwo_2026-10-08_j.log`
(git-ignored) started before the reset.

## 2026-10-08 — First update over Wi-Fi

`logs/yotwo_2026-10-08_j.log`:

- **The new layout starts**: `No factory image, trying OTA 0`, app loaded
  from `0x10000`. Settings intact (233 entries), Wi-Fi and sign-in as
  before.
- **The SD card is back**: mounted first time, 10.9 GB free. The three
  copies are on it and complete. No cards are known (the one card was
  unlinked from the web page earlier in the day).
- **Update over Wi-Fi worked end to end.** Code shown at 494.8 s; upload
  of 1,975,184 bytes began at 502.6 s into `ota_1`; verified and
  accepted at 542.5 s (40 s); restart; `Loaded app from partition at
  offset 0x3d0000`; `in slot ota_1: on trial after an update`; `the new
  firmware is serving its web page: kept` at 4.7 s. Downloads were held
  during the upload.
- One line at each start, `esp_core_dump_flash: Incorrect size of core
  dump image`: the crash-dump area is in a new place and holds whatever
  was there before. Harmless; it is written properly at the next crash.
- Internal memory 41 to 42 KB free.

Changed afterwards: a firmware on trial must also have run for a minute
before it is kept. Built, not flashed (it can now go over Wi-Fi).

The fallback itself (a firmware that fails its trial) has not been
exercised.

## 2026-10-08 — Formatting the SD card from the web page

User asked what should be on the card (it came from the stock player and
carries about 3.9 GB of its files; ours uses only `abs/`), how the stock
firmware treats a blank SD card (it formats it itself; I first took the
question to mean blank NFC cards), and then for a format button on the web
page. Built: `storage_format()`, `/api/format/code` and `/api/format`
behind the display code, a section on the Files tab; linked cards are
written back after the format. Built (1.98 MB), tests pass. Not flashed;
it can go over Wi-Fi.

## 2026-10-08 — First OTA session's full log: updates and format both work; watchdog during downloads

`logs/yotwo_2026-10-08_j.log` (the thirty-first flash's capture, finished):

- **Two firmware updates over Wi-Fi, both good.** 606533b accepted into
  ota_1, kept; then a53ed72 accepted into ota_0, kept (slots alternate).
  The one-minute trial held.
- **The SD format works end to end.** Asked from the web page, code
  shown, `formatting the SD card` → `SD card formatted` (~12 s), restart,
  card now `15189 MB free of 15189` (wiped clean — the old Yoto content
  and `abs` both gone), then the three books re-downloaded.
- **Task-watchdog warnings during the re-download** (7, 15 s apart,
  `IDLE0 (CPU 0)` starved, `download` running). Warnings only
  (`ESP_TASK_WDT_PANIC` off): the player ran through them and every
  download completed. Cause: the writer holds core 0 through each segment
  (the SD driver polls) and the higher-priority TCP/IP task competes, so
  the core-0 idle task can miss its 15 s window during a flat-out,
  back-to-back re-download onto a fresh card (a worst case, not normal
  one-at-a-time fetching). Change (0.1.1-beta): the writer rests a tick
  every 64 KB, not just once per segment. Should reduce them; a single
  blocking SD stall on a cheap card could still trip it, which is
  harmless here.

## 2026-10-08 — 0.1.1-beta

First patch build under the new scheme. `download.c`: a yield every 64 KB
in the SD write loop (watchdog noise during heavy downloads). Built, not
flashed. `firmware/version.txt` = 0.1.1-beta.

## 2026-10-08 — 0.2.0-beta: the LED ring (nightlight and wake light)

User asked for the LED ring as on the stock player: face-down turns the
display off and lights the ring, colour and brightness settable on the
player and the web page; and a wake light rising warm orange with the
clock's sunrise, held 5-15 min, cancelled by any button, turned on or
off in both places.

From the stock firmware: the accelerometer is read at I2C `0x18`, and
only its Z axis, with `z > 12000` raw meaning "on front" and `z < -15000`
"on back"; used the same here.

Built: `ring.c` (LEDC PWM on 12/23/19, gamma), `accel.c` (LIS2DH12,
face-down with a second's hold), the ring logic and `SET` → `LITE` in
`ui_core.c`, host settings in `ui.c`, the shared settings JSON, and a
Light section on the web page. 15 new scenario tests (81 in all);
setup-page test now lets that page leave some player settings to the
web page. Two things to confirm on the player: the ring's polarity
(`RING_ACTIVE_LOW`), and that face-down really is `z > +12000` on this
board (the log prints the pose and reading whenever it changes).
Version 0.2.0-beta (minor: new feature). Not flashed.

## 2026-10-09 — 0.2.1-beta: Wi-Fi TX power cap for the brownout loop

Cable-flashed 0.2.0-beta (log `yotwo_2026-10-08_n.log`) to isolate the
"stuck at boot logo" from the OTA path. Definitive: it is **brownout**,
not a firmware hang. 0.2.0 boots fully every time (ring, accel, SD,
catalog, time, IP) and then hits `E BOD` (brownout detector) right at
"asking the network for the time" — the Wi-Fi association transmit spike
— and resets, in a loop. No crash/Guru/backtrace. Same power fault seen
intermittently all session (plug/charge lines flapping; battery is a
used 18650 and has weakened over a long day of power-cycling).

Fix (0.2.1-beta): `esp_wifi_set_max_tx_power(60)` = 15 dBm (default 20),
in `wifi.c` via `local.patch`, to cut the association current spike.
Ample for home Wi-Fi. This is a mitigation; the real fix is power-side
(full charge, good cable/charger, and likely a new cell). Built,
archived, not flashed — the player can't OTA while brownout-looping, so
this needs a cable flash once the battery is charged.

Still pending (for when the server + power are stable): offline fallback
when the ABS server is unreachable (don't hang on boot), OTA trial
confirmation independent of the media server, and re-adding the
websocket proxy rule scoped to the Audiobookshelf route only.

## 2026-10-09 — Brownouts persist: suspect the USB-C power mod

Brownouts continued. The player's power input is not stock: the
original 5 V / 3 A pogo-pin dock input was replaced with a USB-C port.
The board has no USB-PD/Type-C controller (the stock dock never needed
one), so the port itself must present 5.1 kΩ pull-downs on **both** CC1
and CC2, or a USB-C charger with a C-to-C cable never turns VBUS on (and
a single resistor makes it work in one plug orientation only). That
would leave the player on a weak battery, matching the "plug status"
line flapping seen in earlier logs. Other suspects: thin or long wires
from the port to the old pogo pads (voltage sag under the Wi-Fi + amp
peak), or a bad joint. Checks to make with a meter are listed in the
reply of this date; firmware unchanged.

## 2026-10-09 — Release and flash scripts

0.2.1-beta is on the player (flashed by the user). At their request:
`tools/release.sh` archives a build into `firmware/releases/` as
`yotwo-<version>.bin` plus `yotwo-latest.bin` (refusing a build whose
embedded version doesn't match `version.txt`, or overwriting an archived
version with different bytes), copies the three boot files to
`releases/boot/`, and prints the RELEASES.md row. `tools/flash.sh`
cable-flashes the four regions from `releases/` — latest by default,
`--version <ver>`, `--list`, `--port`; it auto-picks the single
`/dev/cu.usbserial-*`, refuses if the port is held open, and prints the
version read from inside the image before writing.

## 2026-10-09 — 0.3.0-beta: making bad updates recoverable

Gaps found in the update path: the trial deadline and the keep check
both ran in the main loop (a hang there left the player frozen until the
battery came out); "kept" only meant the web server had been started;
no way back once kept; nothing to stop an untested build or a build that
changes the bootloader/partition table going out over Wi-Fi.

Changes:
- `ota.c`: the five-minute deadline is its own `esp_timer`. Keep needs
  ≥ 60 s uptime, a web request answered (`web_check()`: the browser's,
  or the player asking itself over loopback with the page's headers),
  and the UI task to have drawn within 3 s (`ui_alive_us()`). No media
  server involved. `ota_init()` moved to straight after `board_init()`.
- Going back: `ota_go_back()` (on trial: mark invalid; kept: set the
  other slot to boot, which puts it on trial). The other slot is offered
  only if fully verified (`esp_image_verify`), of this project, and not
  INVALID/ABORTED; checked once, 10 s after start-up.
  Web page "Go back to <ver>" (`POST /api/update/back`, code-confirmed);
  both knobs held 3 s at power-on (`BACK`/`NONE`); automatic after 3
  crash resets in a row within 2 min of start (RTC_NOINIT counter).
- Web page reads the version from the chosen file and follows the
  restart: on trial → kept, or "went back".
- `release.sh`: runs all desktop tests first; warns at ≥ 90 % of the
  slot; compares bootloader (segments, build-time field blanked, checked
  against deliberate one-byte edits) and partition table with
  `releases/boot/` and flags CABLE ONLY.
- Tests: three more request-check cases (self-request, go-back from page
  / from another site) and a route-count check against
  `max_uri_handlers` (now 19 for 17 routes).

Built, all tests pass, archived as 0.3.0-beta (boot files unchanged, so
it can go over Wi-Fi). Not yet on the player. To prove it there: OTA it
and watch it be kept; then a deliberately broken build (e.g. one that
hangs the main loop) to see the timer undo it; and "Go back" from the
page.

## 2026-10-09 — The "stuck at boot" is a 0.2.0 regression, not brownout

0.3.0-beta went over Wi-Fi and stuck at the start-up animation. Its own
trial timer undid it after 301 s (`ota: the new firmware has not proved
itself in five minutes`), restarting into 0.2.1 — the new safety net
working as designed. The page had no "Go back" button because 0.3.0
never got as far as serving it.

Log comparison (`yotwo_2026-10-08_h/m/n`, `yotwo_2026-10-09_b`): every
0.1.1 boot logs `catalog: showing N items ... from the SD cache` about
1.2 s after `4 libraries on the server` (from the SD cache in
`catalog_init()`), then `web page up`. **No 0.2.x boot ever does**, with
or without the ring plugged in. The one `E BOD` in 0.2.0's log was
over-read: the brownout diagnosis of 0.2.1 was wrong. The Wi-Fi TX cap
is harmless but did not address it.

Where it stops: `set_libraries()` logs its count and then takes the UI
lock with no timeout (`catalog.c:88`); the animation (drawn by the UI
task under that lock) freezes at the same time. So the UI task is stuck
holding the lock, or another task holds it. No crash, no watchdog (only
CPU0's idle task is watched; the UI task is on CPU1). The 0.2.0 diff
(ring, accel, LITE settings, ring logic in the tick) has nothing that
blocks on its face.

0.3.1-beta adds a stall reporter (`ui.c`): if the UI has not drawn for
8 s, it logs which task holds the UI lock and prints every task's
backtrace (`esp_backtrace_print_all_tasks`), decodable against
`build/yotwo.elf`. Needs a cable flash (OTA to a build that freezes at
start-up cannot be confirmed, and 0.2.1 freezes too).

## 2026-10-09 — Start-up freeze found: the accelerometer task

0.3.1-beta cable-flashed (`yotwo_2026-10-09_c.log`). The stall report
fired 9 s after the last frame: the UI lock is held by the UI task
itself, which is waiting in `spi_device_acquire_bus()` (display.c:32,
via `display_push`). No task holds the shared SPI bus: the NFC task is
asleep between polls (nfc.c:203), everything else idle; `main` waits for
the UI lock in `set_libraries()`. A wake-up in IDF's SPI bus lock that
never came.

Test build `0.3.1-test-noaccel` (identical, `accel_init()` not called;
not archived or tagged, source restored), cable-flashed
(`yotwo_2026-10-09_d.log`): **boots normally** — SD cache at 4.3 s, web
page up.

The accelerometer task had a 2560-byte stack, the smallest of ours (the
others that log have ≥ 3072), and is created straight after the
display's SPI device. Most likely its stack overflowed (a log line on
top of an I2C read) past FreeRTOS's end-of-stack canary into the
display's bus-lock record allocated just before it; that would fit
every 0.2.x start freezing the same way. 0.3.2-beta: stack 4096, and the
task logs its high-water mark ten seconds in to confirm the overflow.

The test build is on the player, works, and has the 0.3.0 update code,
so 0.3.2 can go over Wi-Fi — the first real run of the trial/keep path.

## 2026-10-09 — Correction: the freeze is a display race, not the accelerometer

0.3.2-beta went over Wi-Fi and froze the same way, now with **speckled
colours** in the animation (right columns, noisy and missing colours)
and `WIFI` under it; its trial timed out and it went back to the test
build. The test build without the accelerometer then froze too on that
restart, and started fine after a manual reset: the freeze is
intermittent, and the accelerometer test was a coincidence.
(`yotwo_2026-10-09_e.log`.) Power ruled out: the pogo dock is back.

UI scenarios under AddressSanitizer + UBSan: 81 pass, no errors, so not
`ui_core.c`.

Cause: `power_init()` runs on the **main** task about 2.7 s into
start-up, after `ui_init()` has started the UI task drawing, and calls
`display_set_brightness()`. `display.c` had no lock: both tasks wrote
the one shared transfer buffer `s_tx` (speckles) and both took the same
SPI device's bus lock, which IDF cannot hand from a device to itself,
so the second waits for ever with no holder (the stall report: UI in
`spi_device_acquire_bus`, nothing holding the bus). Every freeze began
3-3.5 s in. The race predates 0.1.1; 0.2.0's start-up timing made it
frequent. The web page's brightness setting is a second path.

Fix (0.3.3-beta): a mutex in `display.c` around `display_push()` and
`display_set_brightness()`. Accelerometer stack stays at 4096 (harmless;
its comment corrected). Also documented: a cable flash's blank
`otadata` forgets that the other slot failed a trial, which is why the
page offered "Go back to 0.3.0-beta" earlier.

## 2026-10-09 — 0.3.3 confirmed; 0.4.0-beta: sign-in moves to the web page

0.3.3-beta over Wi-Fi: user reports it solved the start-up freeze (the
display race).

User request: change the server and account on the web page; the
captive portal should be Wi-Fi setup only.

- `setup/account.c` (new): the server/sign-in logic lifted out of the
  portal's `apply()` — normalise the address, API key or
  username/password (kept encrypted if "remember"), or keep the saved
  sign-in if the server is unchanged and still accepts it. Checked with
  the server before saving. `account_to_json()` never includes secrets.
- Web page: Settings → "Audiobookshelf server" card. `GET /api/account`;
  `POST /api/account/code` (code on the display); `POST /api/account`
  (checks, saves, restarts). Request bodies and the password are wiped
  after use. Route table 20 of 22.
- Setup portal: Wi-Fi only (page, `on_config`, `apply()`); after saving
  it shows the player's `.local` address before restarting.
- First run needs only Wi-Fi (`main.c` no longer uses ABSpresso's
  `config_complete()`). With no sign-in the player scrolls
  `SIGN IN AT <NAME>.LOCAL` and does not call the server; signed out
  says the same.
- `SET` → `SIGN` renamed `WIFI`, with a Wi-Fi icon.
- Tests: `setup_page.sh` now checks the sign-in card both ways (fields
  sent vs read by `web.c`, fields shown vs given by `account.c`).
  All pass. Not yet on the player.

## 2026-10-09 — Bluetooth headphones: tried on a branch, shelved

Trial build 0.5.0-bt.1 on a branch (not published). Measured on the player: keeping Classic
Bluetooth's 56 KB controller reserve leaves Wi-Fi unable to start
(`ESP_ERR_NO_MEM` in `wifi_start`, one receive buffer short); the OTA trial
rolled it back. With ~30-37 KB internal free in normal use, Bluetooth with
Wi-Fi is some 20-40 KB short on this chip, which matches the stock firmware
leaving headphones off for the V2 only. Shelved by the user as a
nice-to-have; untried options noted on the branch. Back on `main`
(0.4.0-beta); `sdkconfig` regenerated and identical to before the branch.
The player's NVS keeps an unused `btaudio`/`on` = 1.

## 2026-10-09 — 0.5.0-beta: day/night brightness settings; sleep-card dim fix

User request: set the day and night brightness; and a bug: after a sleep
card is taken out the display stays at the sleep level.

- Bug: `card_out()` froze the sleep timer but left `s_sleep_dim` set, and
  `card_in()` only set it on the first insert. Now the dim follows the
  card: out restores the usual level, back in dims again (both cases of
  putting it back: carrying on, and the extension after the timer ended).
- Brightness: the fixed 80/40 % became settings, `SET` → `BRT` → `DAY` /
  `NITE` (10-100 % in tens, previewed on the display while choosing), and
  on the web page's Clock section; NVS `ui` / `brday`, `brnight`. The tick
  blends between them with the clock's sunrise and sunset as before; idle
  dim (a quarter after a minute) and the sleep level (3 %) unchanged.
- BRT sits after CLK in SET (the CLK test needed one more step back).
- Tests: `seq_` now ignores brightness lines unless a test asks about them
  (the sleep-card sequence tests broke only because the fix adds lines).
  New: sleep card out restores / back in dims; BRT DAY; BRT NITE; preview.
  UI scenarios 86, all desktop tests pass. Built; not yet on the player.

## 2026-10-09 — 0.5.1-beta: names for other libraries' favourites on the downloads page

User report: downloads page entries named only by an ID
(`b911d8eb-…`), no cover, "not kept" with "Keep again".

What they are: favourites (which span all libraries on the server) from a
library other than the one loaded, which were once removed with the
page's **Remove** button and so sit on the "not kept" list. That part is
as designed. The bug was the page could only name books of the loaded
library or of cards, and only had covers for the loaded library.

Fix: `names.c` (new) keeps titles/authors of such books, looked up on the
server one per offline-task pass (`GET /api/items/<id>`; 404 shows "(no
longer on the server)"), persisted to `abs/names.txt`. The downloads list
asks for names and queues the cover (`cover16_get()`, which fetches any
item's cover by ID). Offline task stack 4 → 8 KB (PSRAM): it now makes a
TLS call. (A suspected double mutex release in `offline_wanted()` turned
out to be an artefact of two overlapping printouts: no change.)
Built, all tests pass; not yet on the player.

## 2026-10-09 — 0.5.2-beta: any button leaves setup; setup-server race fixed

User: SET → WIFI shows the code and "gets stuck"; the right-knob hold did
get out, but asked for any button to leave. Now either knob pressed or
held, both knobs, or the power button leave setup opened from SET (or from
a stuck start-up); turning a knob does not; first-run setup still cannot
be left. A press is ignored while a phone is mid-save
(`host_setup_stop()` now returns false then); start-up of the setup
network no longer reports "busy", so presses during it still leave.

Also fixed a race found while reading the path: `host_setup_start()`
stopped the player's web page, then `portal_start()` only marked setup
active after its own server had started, so the main loop (other core)
could restart our page in between and take port 80, leaving the setup
page unserved. `portal_start()` now marks itself active first, stops our
page itself, and retries its server start for up to a second.
UI tests 89 (3 new). Not yet on the player; no log was captured (adapter
unplugged), so the race is fixed from reading, not observed.

## 2026-10-11 — 0.5.3-beta: clock without the sky glow

User request: a more minimal clock, the sun without the glowing
background. `draw_clock()` now fills black at every hour; the sun and
moon are unchanged (the moon still fades out as the sun rises, now into
black). `docs/media/ui_clock.png` regenerated from the simulator
(6:50, 7:05, 12:00, 19:00, 22:00). All desktop tests pass. Radio streams
discussed and parked by the user (design (a): stations kept on the player,
managed on the web page, cards linked to them).

## 2026-10-11 — 0.5.4-beta: knobs reversed

User: both knobs turn the wrong way. `ENC_REVERSED` in `input.c` 1 → 0
(it had followed the stock config's "reverse" flag for both encoders,
which evidently means the opposite here, or our counter's A/B wiring
differs from the stock firmware's). Affects every turn: volume, lists,
pickers. Built, tests pass; not yet on the player.

## 2026-10-11 — Second player; 0.5.5-beta: display test

A second Gen 2 board, still on stock firmware. Read-only checks before
anything is written: chip **ESP32-D0WD-V3, revision v3.0** (the first
board is v1.0), 8 MB flash from manufacturer `c8` (GigaDevice; the first
is Boya). eFuses: flash encryption off
(`FLASH_CRYPT_CNT` 0), secure boot v1/v2 off, JTAG and download-mode
decrypt not disabled. Full 8 MB backup to `firmware_backup_board2_8MB.bin`
(git-ignored: it holds this board's Yoto credentials).

The user reports some of this board's pixels show only red (under the
stock firmware, so hardware). Added a display test for diagnosing it:
web page Settings → Display → "Test the display" (`POST
/api/display/test`): the whole screen red, green, blue, white for 3 s
each, then a white row sweeping down and a column across (0.25 s a step),
at a fixed 60 % brightness; about 20 s, then the screen comes back.
`ui_core_display_test()`; simulator command `dtest`; 2 new UI tests (91).

Backup of the second board abandoned: its serial link drops bytes far more
often than the first's (a read failed after 57 KB; a failed read leaves the
stub stuck until the board is put back into bootloader mode). Options were
explained (on-chip MD5 comparison against board 1's dump, reading only the
unique settings pages); the user chose to flash without a backup. So this
board cannot be returned to stock: its own settings area (stock `nvs`,
`0x9000`-`0x39000`, with its device certificate and keys) is overwritten.
Flashed 0.5.5-beta with `tools/flash.sh` (four regions; hash verified).

## 2026-10-11 — 0.5.6-beta: two players with the same name

User asked what happens with two players on `yotwo.local`. The mDNS
component renames the later one (`mangle_name()`: `yotwo-2`, `-3`, …),
but our request check (`admit()` → `refusal()` → `host_is_ours()`)
compared the Host header with `web_hostname()`, the configured name, so
the renamed player refused `yotwo-2.local` and was reachable only by IP.
New `web_address()` returns the live mDNS hostname; used by the request
check, the status page's address, the OTA trial's self-request, the
start-up log and the display's sign-in messages. The setup page's closing
message mentions the `-2` possibility (the final name is only known after
the restart). Tests pass. Not yet on either player.

## 2026-10-16 — Prepared for publishing on GitHub

The user asked to publish the project on GitHub, keeping the board details
(pin map, chips, devices) but leaving out everything about the original
firmware's internals.

- **Split.** The study notes on the original firmware, its embedded
  configs, the flash backups, serial logs and the analysis tools are kept in
  a separate private folder with the full development history. This
  repository holds the firmware, the tools, and documentation rewritten for
  readers new to the project.
- **History.** Rebuilt as one commit per release (`v0.1.0-beta` to
  `v0.5.6-beta`), each the source of that release, so every tag still builds
  the version it names. Checked: no Wi-Fi names, server addresses or account
  names in any commit or release file.
- **Docs.** New README (what it does, limits, getting started), and new
  [getting-started.md](getting-started.md) (serial header, boot/reset board
  or jumper, backup, flashing a release or a build, first setup, updating,
  restoring, troubleshooting), with places marked for the author's photos.
  Also new: [using.md](using.md), [building.md](building.md), the public
  [hardware.md](hardware.md), [roadmap.md](roadmap.md) and `CHANGELOG.md`.
- **Tools.**
  - `tools/backup.sh`: a resumable backup in 512 KB pieces, for these noisy
    serial links.
  - `tools/restore.sh`.
  - `tools/verify_backup.py` (was `verify_dump.py`).
  - `tools/flash.sh`: now finds more adapters, falls back to an installed
    esptool, adds `--first` (erases the old settings area), and downloads
    a published release when there is no local build.
  - `tools/cable_zip.sh`: the cable bundle.
  - `tools/publish_release.sh`: creates GitHub releases from the
    changelog.
- Release binaries are no longer in git; they go on GitHub Releases.
