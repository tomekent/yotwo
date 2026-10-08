# Release binaries

Versioned app images for rollback. Each is the `yotwo.bin` from the
commit tagged `v<version>`, kept so a known-good build can be put back
without rebuilding — over Wi-Fi (web page → Settings → Firmware → upload
the `.bin`) or by cable (`0x10000 yotwo-<version>.bin`, with the
bootloader / partition table / `ota_data_initial.bin` from
`docs/journal.md`'s flash notes, which rarely change).

Rebuilding a version from its git tag gives a functionally identical
image but not a byte-identical one (the build date/time is embedded), so
these archived files are the exact artifacts.

| Version | Date | Commit | Size | SHA-256 | Notes |
|---|---|---|---|---|---|
| 0.2.1-beta | 2026-10-09 | (this commit) | 1992736 B | c4ff2b3563e0fe51181997bdcd121730598677237362725437bf5dc543db25b4 | Wi-Fi transmit power capped at 15 dBm to cut the current spike that was browning out a weak battery at boot. |
| 0.2.0-beta | 2026-10-08 | (this commit) | 1992352 B | 589f62dedd77b139dd2a093dafa36bab5cd13ade5afebf1365c3cb2246d79589 | LED ring: face-down nightlight (colour, brightness), sunrise wake light (hold 5-15 min, any button cancels); SET > LITE and web page. Untested on the player. |
| 0.1.1-beta | 2026-10-08 | (this commit) | 1978848 B | 27e4062677b033731a126132858b93fcfca255ae143225fe6879c9e6048331bd | First archived build. Patch over 0.1.0-beta: periodic yield in the SD write loop. OTA update + SD format confirmed working on the player at 0.1.0 level. |

<!-- Older: 0.1.0-beta (v0.1.0-beta) was never flashed; rebuild from its tag if needed. -->
