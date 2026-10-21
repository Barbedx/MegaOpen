# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions follow
`MEGAOPEN_VERSION` in `src/Version.h`.

## [Unreleased]

### Added
- Web page in English: the EN / UA switch in the header (per browser; `?lang=en` in the address too). Words live in `web/i18n/uk.json` and `en.json`; the English file is fetched only by a viewer who picks it. Every screen.

### Changed
- `/api/health` chains carry only `code` (no English `why`): the page has the words in both languages. Огляд no longer tells you to turn the knobs or watch the rings.
- The catalog's names (settings, their choices, signals, Raise rows, layouts, styles, scenes, console) are in the page's dictionaries, so they follow the language switch. `/api/catalog` sends ids: no setting `label`, a `name` only where the id is not a word (`AF-RDS`, `125k`), enum lists name their kind (`dom`), Raise rows a section id and an English unit; `/api/map?meta=1` leaves the label slot empty. No Ukrainian text is left in the firmware outside comments. Signal lists show a name for every signal the Raise list names (they showed its id, `speed`), and doors and radar sensors say which ones.
- `/api/health` `keys.dest`: a destination that cannot take a key says why with a `code` (`uart_off`, `hu_silent`, `no_ams_role`…) in place of an English `why`.
- Error replies are codes, one refusal a line (`pulseMs: number`, `param exp`, `busy`), not English sentences; the page shows them in its language. A refused macro now marks its row again (the reply said `row 3: …`, the page looked for `3: …`).

### Fixed
- Плата › Усі налаштування: a number setting showed its unit twice ("Брязкіт, мс, ms"), and the canbox version (text) was a number box.

## [1.4.0] - 2026-10-21

### Added
- `tools/car_report.sh <board>`: after a drive, one folder and `.tar.gz` with status, health, settings, the settings file, the live map, the logs (ring, this boot, the boot before), the update state and the capture list, plus `summary.txt`. GETs only.
- «Авто — наживо»: «✓ підтвердити» / «↺ скасувати» on a hypothesis row. A confirmed signal loses its «?» everywhere (menu, car screen, alerts, the G-meter ring); «Заводські прив'язки» brings the hypotheses back. `/api/map` `{confirm:{sig,on}}`.
- «Авто — наживо»: a recorder for the car test (record/pause, markers, CSV), with no extra requests to the board.
- RAV4 page: the bonnet goes to the head unit (`0x24` D1 b7). The X84 broadcast has no bonnet contact, so it has no factory binding.

### Changed
- `/api/map`: the map part of a request is all or nothing. A refused confirm or a table that would overflow leaves the saved map as it was; «↺» restores each row's factory flag.
- RAV4 page: the battery row shows the live volts and says protocol 112 has no field for them, with a link to the head-unit battery experiment.

### Fixed
- `/api/health` `clock`: the head unit's clock (`0x9D`, once a minute) counts as a source. It used to say `clock_no_phone` while the head unit kept the panel's clock right. New codes `clock_phone`, `clock_hu`, `clock_no_source` (replace `clock_ok`, `clock_no_phone`), and `src`. With no phone the pane's clock face runs from the head unit's time (it stayed blank).
- CI: the phone-width audit no longer hangs on the live page (the recorder's leave-page warning is attached only while a recording is unsaved); a hung page now fails by name.

## [1.3.0] - 2026-10-21

### Added
- Pane scene G-метр: the felt force from 0x2A0 (left turn → right, braking → up) as a dot with a trail, a 0.5 g ring and a 3 s peak; the ring dotted while the decode is unverified, a still dotted ring with no fresh data. Not in the carousel.
- Car screen preset «Поїздка»: consumption now/average, range, trip distance (`--` with no data; the trip keeps its last value with `?` after the engine stops); Display › Car screen › Preset, `carPreset`, and a card on Екран.
- Head unit experiment: battery volts on the tablet (off by default). The RAV4 profile has no battery field, so the volts ×10 go in a borrowed one: the range of `0x21`, `0x7D 04` D4-D5, or the current value of `0x23`. A field another experiment uses is refused (400, in Ukrainian on the page).
- `/api/health` `hu.climateChanges`: changed `0x28` frames since boot, to compare with climate pop-ups seen in the car.
- CI job `ui`: the web app audited at phone width (390 and 360 px, light and dark) on every push. It fails on a page wider than the screen, text under 4.5:1 contrast, a control under 40 px, or a console error; findings show as annotations on the run page.

### Changed
- Panel menu: the car screen settings moved into Display › Car screen.
- AffaDisplay v1.3.0 (from v1.1.1). The dev-only `/api/display/inforaw` (glass sweep) sends through the library's `showInfoRow`: the same bytes as before, and the hand-built frame (`display/InfoRaw.h`) is gone. `rows=3` puts the ruler in all three info rows (sweep step 15).
- CI and release workflow actions on their current major versions (Node 24).

### Fixed
- AffaDisplay v1.1.2: urgent panel jobs (power on/off) go out in the order they were queued; two of them used to go out reversed.
- Head unit: the tablet's climate overlay no longer pops up by itself. The outside temperature in `0x28` changes only after a new reading has held for 60 s (a flickering degree changed the frame every 5 s), and the 5-byte A/C form goes out only from a measured or pinned signal, not a guessed one.
- Кнопки: the macro card fits a 360 px phone (it overflowed by 13 px with Linux fonts).

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
