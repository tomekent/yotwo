# Release binaries

The release files themselves are on
[GitHub Releases](https://github.com/tomekent/yotwo/releases), published by
`tools/publish_release.sh`; this table records each one's size and SHA-256
so a download can be checked. What changed in each is in
[CHANGELOG.md](../../CHANGELOG.md).

- `tools/release.sh` (after a build) runs the desktop tests (refusing on a
  failure), archives the image here as `yotwo-<version>.bin` and
  `yotwo-latest.bin` (both git-ignored), makes the cable bundle, refreshes
  `boot/`, and prints the row for the table below. It warns if the build's
  bootloader or partition table differ from `boot/`: such a build is
  **cable only**.
- `tools/flash.sh` flashes the newest local build, or downloads a published
  release; `--version`, `--list`, `--port`, `--first`.
- `boot/` holds the bootloader, partition table and blank otadata that every
  cable flash also writes. They have been the same since the two-slot layout
  (0.1.0-beta), so they suit every release.

Rebuilding a version from its tag gives a functionally identical image but
not a byte-identical one (the build date and time are embedded), so the
published files are the exact artifacts.

| Version | Date | Tag | Size | SHA-256 | Notes |
|---|---|---|---|---|---|
| 0.5.6-beta | 2026-10-11 | v0.5.6-beta | 2003376 B | ff0b7206f828a964efdb0cbf2e1b0aab7b77a694119719c07baefde941b4b80e | Two players with one name: the web page now answers to the name mDNS actually gives it (e.g. yotwo-2.local). OTA-able. |
| 0.5.5-beta | 2026-10-11 | v0.5.5-beta | 2003104 B | 1a076f30283be2a1996a0a30968bd1449172c3f9f27dc368cac1afdbf591d563 | Display test on the web page (solid R/G/B/W, then row and column sweeps). OTA-able. |
| 0.5.4-beta | 2026-10-11 | v0.5.4-beta | 2002032 B | 082dbb74e15e9e061ee8c5d59757e9d9f40e9c0e370c882ce18e7356f1eb09c5 | Both knobs reversed (turned the wrong way). OTA-able. |
| 0.5.3-beta | 2026-10-11 | v0.5.3-beta | 2002032 B | 4a04fad3651154934f97c02cb469112381521a861e7ca1940bd00dbb8769f86b | Clock: no sky glow, just the sun and moon on black. OTA-able. |
| 0.5.2-beta | 2026-10-09 | v0.5.2-beta | 2002080 B | 362141a9b8fb99994f51abcbc24af5681ba642bc7725a350767f8ad07254fc78 | Any button leaves setup (SET > WIFI); fix a race that could stop the setup page's server starting. OTA-able. |
| 0.5.1-beta | 2026-10-09 | v0.5.1-beta | 2001904 B | 5043ba552df51c2aafcb5770507b19bf247a061c2a6a817117460b420091612b | Downloads page names and covers for favourites in other libraries (looked up on the server, cached on the SD card). OTA-able. |
| 0.5.0-beta | 2026-10-09 | v0.5.0-beta | 2000048 B | 04d36762510cf93974381a7f6d21cf4ffb63356f3627f4d83ad686f6131bd19b | Day and night display brightness settings (SET > BRT, web page); fix: removing a sleep card restores the brightness. OTA-able. |
| 0.4.0-beta | 2026-10-09 | v0.4.0-beta | 1998656 B | 39ec6bbc22fa1250e6f4e0a49cdd6baeeb8ca1a89b72c0d5acb665f33af811d3 | Server and sign-in on the web page (code-confirmed); setup portal is Wi-Fi only; SET > SIGN renamed WIFI. OTA-able. |
| 0.3.3-beta | 2026-10-09 | v0.3.3-beta | 2000688 B | 0cf84fa195da94ed6bdfffb0aab405f21c9467ef122ddf7428ad59acf7e81a8a | Fix the start-up freeze and speckled frames: display.c serialises its users (main task's power_init brightness raced the UI's frames). OTA-able. |
| 0.3.2-beta | 2026-10-09 | v0.3.2-beta | 2000608 B | 3c09a0759a79b67ea415264a768ce8d562c4d650c2045ba682eb286c424a406c | Accelerometer task stack 2560 -> 4096 (logs its use). Did NOT fix the start-up freeze (wrong cause; failed its trial). |
| 0.3.1-beta | 2026-10-09 | v0.3.1-beta | 2000448 B | 58994915b3b2d3633d0750e75a881b3b4d876d1c84570a1d0857b654d3c5890d | Diagnostic: UI stall reporter (lock holder + all-task backtraces after 8 s without a frame) to find the 0.2.x start-up freeze. Still has the freeze. |
| 0.3.0-beta | 2026-10-09 | v0.3.0-beta | 1999392 B | 27bab9161f9f8e8cc0e2becaa654eede19e37d976a6a34e63f479e668b9d3f44 | Safer updates: trial needs web answer + UI alive, own 5-min timer; go back from the web page, both knobs at power-on, or by itself after 3 crashes. OTA-able (boot files unchanged). |
| 0.2.1-beta | 2026-10-09 | v0.2.1-beta | 1992736 B | c4ff2b3563e0fe51181997bdcd121730598677237362725437bf5dc543db25b4 | Wi-Fi transmit power capped at 15 dBm to cut the current spike that was browning out a weak battery at boot. |
| 0.2.0-beta | 2026-10-08 | v0.2.0-beta | 1992352 B | 589f62dedd77b139dd2a093dafa36bab5cd13ade5afebf1365c3cb2246d79589 | LED ring: face-down nightlight (colour, brightness), sunrise wake light (hold 5-15 min, any button cancels); SET > LITE and web page. Untested on the player. |
| 0.1.1-beta | 2026-10-08 | v0.1.1-beta | 1978848 B | 27e4062677b033731a126132858b93fcfca255ae143225fe6879c9e6048331bd | First archived build. Patch over 0.1.0-beta: periodic yield in the SD write loop. OTA update + SD format confirmed working on the player at 0.1.0 level. |

0.1.0-beta (tag `v0.1.0-beta`) was never released; build it from its tag if needed.
