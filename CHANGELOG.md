# Changelog

Each release's `yotwo-<version>.bin` can be installed over Wi-Fi from the
player's web page (Settings → Firmware). The cable bundle is for a first
install. Every release so far uses the same bootloader and partition table,
so any of them can be installed over Wi-Fi on a player already running
yotwo.

## 0.5.6-beta

- **Two players on one network:** the second one is named `yotwo-2.local`
  (then `-3`, …) automatically, and its web page now works at that name.
  Before, it was reachable only by IP address.

## 0.5.5-beta

- **Display test** on the web page (Settings → Display → Test the display):
  the whole screen red, green, blue and white, then a row and a column
  sweeping across. Handy for spotting pixels with a dead colour.

## 0.5.4-beta

- Both knobs' turning direction reversed: they turned the wrong way.

## 0.5.3-beta

- Clock: no sky glow behind the sun and moon, just the sun or moon on black.

## 0.5.2-beta

- `SET` → `WIFI`: any button now leaves the setup screen.
- Fixed a race that could stop the setup page's server from starting.

## 0.5.1-beta

- Downloads page: names and covers for favourites from other libraries
  (looked up on the server, then remembered on the SD card), instead of a
  bare ID.

## 0.5.0-beta

- **Day and night brightness** settings, on the player (`SET` → `BRT`) and
  the web page.
- Fixed: taking a sleep card out now restores the normal brightness.

## 0.4.0-beta

- **Audiobookshelf sign-in moves to the web page** (Settings →
  Audiobookshelf server), confirmed with a code on the display. The phone
  setup network is now for Wi-Fi only.
- `SET` → `SIGN` is renamed `WIFI`.

## 0.3.3-beta

- **Fixed the freeze at the start-up animation** (and speckled frames) seen
  since 0.2.0: two parts of the firmware were writing to the display at once.

## 0.3.2-beta

Diagnostic build while chasing the start-up freeze; it still has the freeze.
Use 0.3.3 or later.

## 0.3.1-beta

Diagnostic build that reports a stalled display with task backtraces; it still
has the start-up freeze. Use 0.3.3 or later.

## 0.3.0-beta

- **Safer updates.** A new version has to show it works (the web page
  answers, the display is running) within its five-minute trial, or the
  player goes back to the previous one.
- Going back by hand: from the web page, or by holding both knobs while
  switching on. The player also goes back by itself after three crashes in a
  row.

Known issue: can freeze at the start-up animation (fixed in 0.3.3).

## 0.2.1-beta

- Wi-Fi transmit power capped at 15 dBm, to reduce the current spike that
  could brown out a weak battery at start-up.

Known issue: can freeze at the start-up animation (fixed in 0.3.3).

## 0.2.0-beta

- **LED ring:** a nightlight when the player lies face down (colour and
  brightness settable), and a sunrise wake light (held for 5–15 minutes; any
  button turns it off). On the player under `SET` → `LITE`, and on the web
  page.

Known issue: can freeze at the start-up animation (fixed in 0.3.3).

## 0.1.1-beta

First published build. Streaming from Audiobookshelf, browsing (recent, new,
favourites, downloaded, A–Z, authors, series), cover art, favourites mirrored
to the server, downloads and offline mode, book and sleep cards, the clock,
day/night brightness, volume limit, automatic switch-off, the web page, and
updates over Wi-Fi.
