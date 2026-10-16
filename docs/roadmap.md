# Roadmap

yotwo is in beta: it is in daily use, but the UX still needs refining, and it
needs more testing on more players (and more kids).

## Done

- Audiobookshelf library on the player: browse by recent, new, favourites,
  downloaded, A–Z, author, series; switch library; cover art at 16 × 16
- Streaming playback with chapters, sleep timer, volume limit
- Favourites mirrored to an Audiobookshelf collection
- Downloads to the SD card, offline mode, progress synced on reconnection
- NFC cards by UID: book cards and sleep cards
- Clock with sunrise and sunset; LED ring nightlight and wake light
- Day/night brightness, idle dimming, automatic switch-off
- Battery display, headphone detection, face-down detection
- Wi-Fi setup from a phone; a web page for everything else
- Over-the-air updates with a trial run and automatic roll-back
- Desktop simulator and tests for the UI and web page

## Next

- [ ] Podcast downloads (episodes are streamed only for now)
- [ ] Faster browsing in long lists (acceleration or letter jumps)
- [ ] Alarms
- [ ] Automatic brightness from the light sensor (wired, not yet read)
- [ ] A factory-reset gesture (wipe Wi-Fi, sign-in and cards from the player)
- [ ] Low-battery warning and a clean shutdown on a flat battery
- [ ] Updates offered from GitHub Releases on the web page, instead of
      uploading a file
- [ ] Easier first install, e.g. flashing from the browser

## Ideas, parked

- **Internet radio** stations kept on the player and managed on the web
  page, playable from the menu or a card.
- **More card modes:** games, a parent/admin card.
- **Bluetooth headphones:** tried, and shelved. With Classic Bluetooth's
  memory reserved, Wi-Fi can no longer start on this chip. It would need Wi-Fi
  off while Bluetooth is on, or a large memory diet. A Bluetooth *speaker*
  mode (player as the speaker) may be more realistic.
- **Gen 3 players:** untested. The hardware differs (see
  [hardware.md](hardware.md)), and it may be more locked down.

## Known issues

- The serial link on these boards is noisy: flashing and backups only work
  at 115200 baud, and long reads sometimes need retrying.
- Signing in with a username and password: Audiobookshelf ends sessions after
  a while. The player can sign itself back in if allowed to keep the
  password, but an API key avoids the problem.
- Authors are grouped by the whole author string ("A, B" rather than under
  each), and a book in several series is filed under the first.
