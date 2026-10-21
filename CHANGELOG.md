# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions follow
`MEGAOPEN_VERSION` in `src/Version.h`.

## [Unreleased]

### Fixed
- AffaDisplay v1.1.2: urgent panel jobs (power on/off) go out in the order they were queued; two of them used to go out reversed.

## [1.2.0] - 2026-10-21

### Added
- Settings to a file and back: GET/POST `/api/config` and a card on Плата; a dry run shows what changes before anything applies. Passwords never go in the file.
- SK6812 RGBW LED strips: a third strip type, white from min(r, g, b).
- Panel bus watch: the panel link listens 3 s from the first poll before it sends; any other radio's sync keeps MegaOpen silent (health `panel_foreign_master`), and it re-detects 30 s after the syncs stop. A lone cluster reads `panel_cluster_only`. First registration comes ~3 s later than before.
- Health: `hu_dialect_vw` when the head unit speaks the VW canbox profile instead of Toyota, with the 0xF3 "not supported" count.
- Glass sweeps: a 10-minute camera procedure (`docs/glass-sweeps.md`) with a step-through on Сервіс; a developer-only `/api/display/inforaw` row with a declared length.
- `SECURITY.md`: what is exposed and how to report; README and the user guide say to change the AP password at first boot.
- `docs/RELEASING.md`: how a release goes.
- `tools/rc_smoke.sh`: a release-candidate smoke test against a running board (reads and dry runs only).

### Changed
- Requests from other sites are refused: every change needs the header `X-MegaOpen: 1` and the board answers only to its IP or its own `.local` name. `/update` leads to the Плата page; a WiFi scan is a POST.
- POST `/api/settings` lists every refusal in its 400 and applies nothing; `?dry=1` checks without applying.
- Phone layout: 40 px touch targets, whole select labels below 560 px, 4.5:1 contrast in both themes.
- Errors and reasons on the page in Ukrainian.
- The page polls only what is on screen; Огляд every 2 s.
- The 32 kB log ring and request-only buffers live in PSRAM; idle internal heap ~99 kB (was ~58).
- AffaDisplay v1.1.1 (docs and examples only; same firmware).
- README: flashing and building from a clean machine (esptool 5 spelling, download mode, checksums, Windows path limit).

### Fixed
- `t2can_usbhid` builds on Windows again (the compile line passed the 32 kB limit).
- A web key sent while four others wait for the panel's verdict is logged as unwatched; before, its refusal went unlogged.

## [1.1.0] - 2026-10-21

### Added
- Firmware updates from GitHub Releases on the Плата page (T-2CAN): check, download the env's image, verify its SHA-256 against `SHA256SUMS`, reboot into the OTA trial from the page or once the car stands. Never automatic, never while driving.
- Media styles tape (a cassette with turning reels) and typewriter; pane scenes tape and tunnel.
- Volume keys fall back to the phone while the head unit is silent (not in the first minute after boot).
- Direct tier: web screens for the 3x3 key matrix and the KEY1/KEY2 ladder.
- Head unit: a PRND probe on 0x7D 06 behind a switch, with a web sweep.
- `uartBaud` in the settings registry: a list of canbox rates, applied at reboot; a stored rate outside the list can be repaired.
- User guide (`docs/USER-GUIDE.md`): first boot, the panel menu, live car page and alerts, media, keys, updates, health codes.

### Changed
- AffaDisplay v1.1.0: its fullscreen sends the OEM geometry; MegaOpen's own fullscreen sender is gone.
- Car screen and the media Fullscreen layout use the OEM fullscreen geometry (row 1 no longer indented, row 3 gets room).
- Media rows and the header fit the glass's 22 columns.
- Key routing knows what each sink can do; the AMS default no longer takes the phone class.
- AMS volume: commands per press measured from the phone's volume instead of a fixed 15.
- No automatic screen covers a live popup, in any layout.
- The generated web page is no longer tracked, so a build cannot mark the tree dirty.

### Fixed
- Steering angle centred on 0x7FFF, as the X84 matrix says.
- A Renault 2-bit state's invalid value no longer reads as "yes"; belts and the passenger seat read 1 = yes.
- AMS reads the whole of a long title, artist or album; a failed or short read never replaces the shown title.
- BLE: AMS long reads and ANCS attribute requests wait for idle keys; the AMS caches and bonds are cleared only on the BLE task.
- Panel menu: a Bool pressed twice toggles twice; a knob edit from the menu is no longer lost to a web save.
- Car screen: a "none" row stays blank in its place instead of moving the rows below it up.
- Simulator: GET /api/settings no longer fails on a text setting.

## [1.0.0]

### Added
- LilyGO T-2CAN firmware: AFFA/Carminat panel on CAN B, car on CAN A (listen-only), Raise/RAV4 canbox to the head unit over UART.
- iPhone over BLE: now playing (AMS), notifications (ANCS), clock (CTS).
- Two rotary knobs with WS2812 rings on QWIIC.
- Car CAN decode for the X84 broadcast rows, with gear, consumption and range derived.
- Live car page (`#live`) with per-signal age and verified state.
- Panel alert rules: a popup on the display when a signal crosses a threshold.
- Car screen: three CAN signals on the panel instead of now-playing.
- Panel menu: hold the right knob to browse settings and car data on the display.
- Settings registry: every setting described once, shared by the web app, the panel menu and `/api/settings`.
- Head-unit experiments behind switches.
- OTA trial: a new image confirms itself after 60 s healthy and rolls back otherwise.
- `env:direct`: ESP32 DevKit V1 without a panel (key matrix + resistor ladder).
- CI on every push and pull request; tagged releases with OTA and factory images per env.

### Changed
- The panel link is the AffaDisplay library's (`affa::TwaiLink`); renders go through it from any task.
- Capture settings, log level, key routing, head-unit message switches and Raise tuning moved into `/api/settings`; their own routes are gone.
- Panel alert rules moved from `/api/map` to `/api/settings`.

### Removed
- DevKit + board V setup, the runtime CAN role and the bench CAN diagnostics.
- CloudLink and `cloud/`.
- OTA password; the WiFi password protects the board.

[Unreleased]: https://github.com/Barbedx/MegaOpen/compare/v1.2.0...HEAD
[1.2.0]: https://github.com/Barbedx/MegaOpen/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/Barbedx/MegaOpen/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/Barbedx/MegaOpen/releases/tag/v1.0.0
