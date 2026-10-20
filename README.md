# MegaOpen

Firmware that sits between a Renault Mégane II and an aftermarket Android head
unit. It acts as the car's radio for the OEM Carminat/AFFA display, and it acts as
a canbox for the head unit.

* **Display (CAN B).** MegaOpen runs the AFFA registration handshake, draws on the
  panel (now playing, clock, a 48×48 picture pane) and takes the steering-column
  stalk's key presses from it. The panel driver is the
  [AffaDisplay](https://github.com/Barbedx/AffaDisplay) library.
* **Head unit (UART).** It speaks the Raise/canbox **Toyota RAV4** profile:
  `0x20` key frames and the vehicle telemetry the profile defines, at 38400 8N1.
  The head unit's own media text comes back the other way and is shown on the panel.
* **Car (CAN A).** The car bus is read **listen-only** for the sniffer, the
  capture recorder and the RAV4 telemetry map. Nothing is transmitted on it.
* **Phone (BLE).** An iPhone pairs as a peripheral, and the firmware reads now
  playing (AMS), notifications (ANCS) and the clock (CTS) from it.
* **Knobs.** Two rotary encoders with WS2812 rings, on I2C through a PCF8574.
* **WiFi.** A web app for setup, live logs, the CAN tools and OTA updates, opened
  from a phone.

Keys always win: the UART lanes give key frames priority over telemetry.

## Hardware

The board in the car is a **LilyGO T-2CAN** (ESP32-S3 N16R8, two isolated CAN
ports). CAN B is the S3's own TWAI on GPIO7/6 and goes to the display. CAN A is an
MCP2518FD on SPI and goes to the car. The head unit is on QWIIC 1 (GPIO43/44).
Pins are named in one place only, `src/board/BoardProfile.h`. Wiring drawings:
[hardware/schematic/t2can-wiring.artifact.html](hardware/schematic/t2can-wiring.artifact.html) and
[hardware/schematic/t2can-qwiic.artifact.html](hardware/schematic/t2can-qwiic.artifact.html).

An ESP32 DevKit V1 build without the panel is also kept (`env:direct`). It reads
the keypad's own wires and listens to the car on its single TWAI port.

## Build

[PlatformIO](https://platformio.org/). Every dependency is pinned to an exact
version or commit in `platformio.ini`, so a fresh clone builds the same image;
the one exception is AffaDisplay, which follows its `release/next` branch until
that is released.

```
pio run                      # env:t2can, the default
pio run -e t2can -t upload   # first flash over USB (hold BOOT if needed)
pio test -e native           # host tests for the portable logic
```

| env | what |
| --- | --- |
| `t2can` | T-2CAN: panel + car + head unit + BLE + knobs |
| `t2can_usbhid` | `t2can` with a USB HID keyboard to the head unit (compile check) |
| `direct` | ESP32 DevKit V1 without a panel: 6-wire key matrix + KEY1/KEY2 resistor ladder ([docs/resistive-keys.md](docs/resistive-keys.md), [docs/wiring-direct.md](docs/wiring-direct.md)) |
| `native` | host tests |

The web app in `web/` is gzipped into the firmware by `tools/gen_pages.py`
before every build. `py tools/build_sim.py` builds a browser simulator of it
with mock data, in `build/`.

## Flashing

After the first USB flash, updates go over WiFi, from `/update` in a browser or:

```
curl "http://<ip>/ota/start?mode=fr&hash=<md5>"
curl -F "file=@.pio/build/t2can/firmware.bin" "http://<ip>/ota/upload"
```

OTA has no password of its own. Neither does the web API, so the WiFi password
is what keeps strangers off the board.

A new image boots on trial and confirms itself only after its health checks pass.
Otherwise the next reboot rolls back.

## Secrets

Copy `src/secrets.example.h` to `src/secrets.h` (gitignored) and set the AP
password and, optionally, your WiFi network. These are the defaults a fresh or
factory-reset board starts with; anything saved from the web app wins.

## Web UI

Without a WiFi network the board starts an AP called `MegaOpen-XXXX`. Its
password is `megaopen` unless `src/secrets.h` says otherwise, so **set it**. The app is
served at `http://192.168.4.1/`, or at the board's IP on your network. It has six
screens: overview (health of every chain), display, keys (mapping, knobs, macros),
car (the RAV4 map, sniffer, capture), board (WiFi, BLE, flash log) and service
(live log, the display and Raise consoles, CAN diagnostics). Every list in the UI
comes from `GET /api/catalog`, so the firmware is the single source of truth.

## Repository

```
src/        firmware (src/app: tasks; src/display: the AffaDisplay ports; src/proto: Raise)
web/        the web app, and web/sim: mocks for the simulator
test/       host tests (env:native)
tools/      build scripts, CAN capture and correlation tools, rule checks
docs/       protocol notes, design notes (docs/design), the backlog
hardware/   T-2CAN wiring (bezel and case to follow)
```

Further reading: [ARCHITECTURE.md](ARCHITECTURE.md),
[docs/raise-protocol.md](docs/raise-protocol.md),
[docs/megane-can-ids.md](docs/megane-can-ids.md),
[docs/BACKLOG.md](docs/BACKLOG.md).

## Status

Installed and running in a Mégane II with a DUDU7 head unit. Mapping the car's
CAN signals (and KWP diagnostic polling for the ones that are not broadcast) is
ongoing. See the backlog.

> Hobby project. It listens to the car's bus and never writes to it, but you wire
> it into your own car at your own risk. Do not operate the menus while driving.

## License

MIT, see [LICENSE](LICENSE).
