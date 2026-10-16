# Building from source

## Toolchain

- **ESP-IDF v5.5.2**: follow Espressif's
  [installation guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32/get-started/index.html).
  `tools/idf.sh` looks for it in `$IDF_PATH`, else `~/esp/v5.5.2/esp-idf`.
- For the desktop tests and simulator, a C compiler and `python3` (and
  `node`, if present, for one extra check).
- For flashing, [uv](https://docs.astral.sh/uv/) or `pip install esptool`.

## Build

```sh
git clone https://github.com/tomekent/yotwo.git
cd yotwo
tools/idf.sh build
```

The first build fetches the managed components (`esp_audio_codec`,
`esp_jpeg`, `mdns`) and generates `firmware/sdkconfig` from
`firmware/sdkconfig.defaults`. The app is `firmware/build/yotwo.bin`, about
2 MB, in a 3.75 MB slot.

`tools/idf.sh` passes anything to `idf.py` (`size`, `menuconfig`,
`monitor`…), except flash and erase commands. Those go through
`tools/flash.sh`, because the player has to be put into bootloader mode by
hand.

Watching the serial log: `tools/idf.sh monitor -p PORT --no-reset` (115200
baud).

## Desktop tests and the UI simulator

```sh
tools/tests/run_all.sh       # every test; no player or ESP-IDF needed
tools/ui_sim/run.sh          # the 16 x 16 UI in a terminal, driven by commands
```

The tests cover the UI (scenarios through the simulator, around 90 of them),
small units, the favourites scan, the setup page and the web page's request
checks. The simulator runs `firmware/main/ui/ui_core.c` unchanged against a
fake library. It can record image strips like the ones in `docs/media/`; see
the comment at the top of `tools/ui_sim/sim.c` for its commands.

`tools/ui_sim/make_gifs.sh` remakes the animated GIFs in `docs/media/`
(`anim_*.gif`). Each is a scripted walk through one part of the UI, recorded
by `tools/ui_sim/gif.sh` at the player's real frame rate with captions. It
needs ffmpeg.

## Layout

```
firmware/
  main/
    main.c             start-up and the main loop
    hardware/          board layer: IO expander, display, input, NFC, SD, battery,
                       power, LED ring, accelerometer; pins.h is the pin map
    ui/                the two-knob UI (ui_core.c is hardware-free; ui.c hosts it)
    web/               the player's web page and over-the-air updates
    setup/             Wi-Fi setup portal, Audiobookshelf sign-in, settings
    abspresso/         Audiobookshelf client, player and downloads, shared with
                       ABSpresso (don't edit directly; see below)
    cards.c, favourites.c, offline.c, cover16.c, names.c
  partitions.csv       two 3.75 MB app slots for OTA
  sdkconfig.defaults
  version.txt
  releases/            RELEASES.md (hashes) and boot/ (bootloader, partition table)
tools/                 build, flash, backup, release and test scripts
docs/
```

## The shared ABSpresso code

`firmware/main/abspresso/` is copied from
[ABSpresso](https://github.com/tomekent/ABSpresso) by
`tools/sync_abspresso.sh`, and yotwo's changes to it are kept as
`firmware/main/abspresso/local.patch`. To change that code, edit the files,
then run `tools/sync_abspresso.sh --update-patch` to regenerate the patch.
`SOURCE.txt` records which ABSpresso commit the copy came from.

## Flashing your build

```sh
tools/release.sh             # tests, then archives the build in firmware/releases/
tools/flash.sh               # flashes the newest local build; --first on a new player
```

Or upload `firmware/build/yotwo.bin` on the player's web page (Settings →
Firmware), if it already runs yotwo.

`tools/release.sh` warns if the bootloader or partition table differ from
those in `firmware/releases/boot/`. Such a build has to go on by cable, since
an over-the-air update only replaces the app.

## Making a release

1. Bump `firmware/version.txt` (semver with a `-beta` suffix for now: patch
   for fixes, minor for features).
2. `tools/idf.sh build`, then `tools/release.sh`.
3. Add the printed row to `firmware/releases/RELEASES.md`, and a
   `## <version>` section to `CHANGELOG.md`.
4. Commit, then `git tag v<version>` and `git push origin main v<version>`.
5. `tools/publish_release.sh <version>` creates the GitHub release. Its notes
   come from the changelog, and it attaches `yotwo-<version>.bin` (for
   over-the-air updates) and `yotwo-<version>-cable.zip` (for flashing by
   cable).
