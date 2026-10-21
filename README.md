# MegaOpen

[![CI](https://github.com/Barbedx/MegaOpen/actions/workflows/ci.yml/badge.svg)](https://github.com/Barbedx/MegaOpen/actions/workflows/ci.yml)

Firmware that sits between a Renault Mégane II and an aftermarket Android head
unit. It acts as the car's radio for the OEM Carminat/AFFA display and as a
canbox for the head unit.

* **Display (CAN B):** registers with the AFFA panel, draws now playing, the clock
  and a 48×48 picture pane, and reads the stalk keys. Driver:
  [AffaDisplay](https://github.com/Barbedx/AffaDisplay).
* **Head unit (UART):** Raise/canbox **Toyota RAV4** profile at 38400 8N1: key
  frames and vehicle telemetry; the head unit's media text comes back to the panel.
* **Car (CAN A):** read **listen-only** for the live page, alerts and the RAV4
  telemetry. Nothing is ever transmitted on it.
* **Phone (BLE):** iPhone now playing (AMS), notifications (ANCS), clock (CTS).
* **Knobs:** two rotary encoders with WS2812 rings, on I2C through a PCF8574.
* **WiFi:** a web app for setup, live logs, CAN tools and OTA updates.

## Hardware

**LilyGO T-2CAN** (ESP32-S3 N16R8, two isolated CAN ports): CAN B (TWAI, GPIO7/6)
to the display, CAN A (MCP2518FD) to the car, head unit on QWIIC 1 (GPIO43/44).
Pins: `src/board/BoardProfile.h`. Wiring:
[t2can-wiring](hardware/schematic/t2can-wiring.artifact.html),
[t2can-qwiic](hardware/schematic/t2can-qwiic.artifact.html).

`env:direct` is an ESP32 DevKit V1 build without the panel (key matrix + KEY1/KEY2
ladder, [docs/wiring-direct.md](docs/wiring-direct.md)).

## Install / Flash

Download from [Releases](https://github.com/Barbedx/MegaOpen/releases):

| file | use |
| --- | --- |
| `megaopen-<env>-<ver>-factory.bin` | first flash over USB, at 0x0 |
| `megaopen-<env>-<ver>.bin` | OTA updates |
| `SHA256SUMS` | checksums |

`<env>` is `t2can` for the car board. The latest release has run on a board;
`-rcN` pre-releases have not yet.

**First flash (USB).** Connect the T-2CAN's Type-C (hold BOOT if it does not enter
download mode), then either:

```
esptool.py --chip esp32s3 write_flash 0x0 megaopen-t2can-<ver>-factory.bin
```

or the [ESP web flasher](https://espressif.github.io/esptool-js/) in Chrome/Edge:
the file at offset `0x0`. For `direct` use `--chip esp32`.

**Updates (OTA).** Open `http://<board>/update` and upload `megaopen-<env>-<ver>.bin`,
or:

```
curl "http://<board>/ota/start?mode=fr&hash=$(md5sum fw.bin | cut -d' ' -f1)"
curl -F "file=@fw.bin;filename=firmware.bin" "http://<board>/ota/upload"
```

A new image boots on trial: it confirms itself after 60 s healthy, otherwise the
next reboot rolls back.

**After an update** press **«Заводські прив'язки»** on the car map page so new
factory signal rows show up (a saved map keeps the old set).

## First boot

1. Join the board's AP `MegaOpen-XXXX`, password `megaopen`.
2. Open `http://192.168.4.1`, go to the board page and add your WiFi.
3. On your network the board is at `http://megaopen.local` (or the router's IP).

OTA and the web API have no password of their own; the WiFi/AP password is the
only lock. Change the AP password on the board page.

## Build

[PlatformIO](https://platformio.org/). Dependencies are pinned in `platformio.ini`.

```
pio run -e t2can                 # car build
pio run -e t2can -t factory      # + firmware-factory.bin for 0x0
pio test -e native               # host tests
```

Envs: `t2can`, `t2can_usbhid` (USB HID keyboard to the head unit), `direct`,
`native`. Default secrets come from `src/secrets.example.h`; copy it to
`src/secrets.h` (gitignored) to change the default AP password or preset a WiFi
network.

## Docs

[ARCHITECTURE.md](ARCHITECTURE.md) · [CHANGELOG.md](CHANGELOG.md) ·
[docs/raise-protocol.md](docs/raise-protocol.md) ·
[docs/megane-can-ids.md](docs/megane-can-ids.md) ·
[docs/BACKLOG.md](docs/BACKLOG.md)

> Hobby project. It never writes to the car's bus, but you wire it into your own
> car at your own risk. Do not operate the menus while driving.

## License

MIT, see [LICENSE](LICENSE).
