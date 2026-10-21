# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions follow
`MEGAOPEN_VERSION` in `src/Version.h`.

## [Unreleased]

### Added
- Firmware updates from GitHub Releases on the Плата page (T-2CAN): check, then download the env's image, check its SHA-256 against `SHA256SUMS`, and reboot into the OTA trial from the page or once the car stands. Never automatic, never while driving.

### Changed
- AffaDisplay v1.1.0: its fullscreen sends the OEM geometry; MegaOpen's own fullscreen sender is gone.
- Car screen and the media Fullscreen layout now use the OEM fullscreen geometry (row 1 no longer indented, row 3 gets room).
- Car screen: a "none" row stays blank in its place instead of moving the rows below it up.

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

[Unreleased]: https://github.com/Barbedx/MegaOpen/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/Barbedx/MegaOpen/releases/tag/v1.0.0
