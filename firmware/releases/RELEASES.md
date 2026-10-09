# Release binaries

Versioned app images for rollback. Each is the `yotwo.bin` from the
commit tagged `v<version>`, kept so a known-good build can be put back
without rebuilding — over Wi-Fi (web page → Settings → Firmware → upload
the `.bin`) or by cable with `tools/flash.sh`.

- `tools/release.sh` (after a build) runs the desktop tests (refusing
  on a failure), copies the image here as `yotwo-<version>.bin` **and**
  `yotwo-latest.bin`, refreshes `boot/`, and prints the row for the
  table below. It warns if the build's bootloader or partition table
  differ from `boot/`: such a build is **cable only**.
- `tools/flash.sh` flashes `yotwo-latest.bin`; `--version 0.2.0-beta`
  for another, `--list` for what's here, `--port` if more than one
  adapter. Player must be in bootloader mode first.
- `boot/` holds the bootloader, partition table and blank otadata that
  every cable flash also writes. They have been the same since the
  two-slot layout (0.1.0-beta), so they suit every release here.
- `yotwo-latest.bin` is a convenience copy, not tracked in git.

Rebuilding a version from its git tag gives a functionally identical
image but not a byte-identical one (the build date/time is embedded), so
these archived files are the exact artifacts.

| Version | Date | Commit | Size | SHA-256 | Notes |
|---|---|---|---|---|---|
| 0.4.0-beta | 2026-10-09 | (this commit) | 1998656 B | 39ec6bbc22fa1250e6f4e0a49cdd6baeeb8ca1a89b72c0d5acb665f33af811d3 | Server and sign-in on the web page (code-confirmed); setup portal is Wi-Fi only; SET > SIGN renamed WIFI. OTA-able. |
| 0.3.3-beta | 2026-10-09 | (this commit) | 2000688 B | 0cf84fa195da94ed6bdfffb0aab405f21c9467ef122ddf7428ad59acf7e81a8a | Fix the start-up freeze and speckled frames: display.c serialises its users (main task's power_init brightness raced the UI's frames). OTA-able. |
| 0.3.2-beta | 2026-10-09 | (this commit) | 2000608 B | 3c09a0759a79b67ea415264a768ce8d562c4d650c2045ba682eb286c424a406c | Accelerometer task stack 2560 -> 4096 (logs its use). Did NOT fix the start-up freeze (wrong cause; failed its trial). |
| 0.3.1-beta | 2026-10-09 | (this commit) | 2000448 B | 58994915b3b2d3633d0750e75a881b3b4d876d1c84570a1d0857b654d3c5890d | Diagnostic: UI stall reporter (lock holder + all-task backtraces after 8 s without a frame) to find the 0.2.x start-up freeze. Still has the freeze. |
| 0.3.0-beta | 2026-10-09 | (this commit) | 1999392 B | 27bab9161f9f8e8cc0e2becaa654eede19e37d976a6a34e63f479e668b9d3f44 | Safer updates: trial needs web answer + UI alive, own 5-min timer; go back from the web page, both knobs at power-on, or by itself after 3 crashes. OTA-able (boot files unchanged). |
| 0.2.1-beta | 2026-10-09 | (this commit) | 1992736 B | c4ff2b3563e0fe51181997bdcd121730598677237362725437bf5dc543db25b4 | Wi-Fi transmit power capped at 15 dBm to cut the current spike that was browning out a weak battery at boot. |
| 0.2.0-beta | 2026-10-08 | (this commit) | 1992352 B | 589f62dedd77b139dd2a093dafa36bab5cd13ade5afebf1365c3cb2246d79589 | LED ring: face-down nightlight (colour, brightness), sunrise wake light (hold 5-15 min, any button cancels); SET > LITE and web page. Untested on the player. |
| 0.1.1-beta | 2026-10-08 | (this commit) | 1978848 B | 27e4062677b033731a126132858b93fcfca255ae143225fe6879c9e6048331bd | First archived build. Patch over 0.1.0-beta: periodic yield in the SD write loop. OTA update + SD format confirmed working on the player at 0.1.0 level. |

<!-- Older: 0.1.0-beta (v0.1.0-beta) was never flashed; rebuild from its tag if needed. -->
