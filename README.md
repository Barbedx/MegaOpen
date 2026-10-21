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
[t2can-qwiic](hardware/schematic/t2can-qwiic.artifact.html) (HTML pages: GitHub
shows their source, so download them and open them in a browser).

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
`-rcN` pre-releases have not yet. Their files carry the version without
`-rc` on purpose (`v1.0.0-rc2` holds `megaopen-t2can-1.0.0.bin`): the release
reuses the last rc's files byte for byte. Check a download with
`sha256sum -c SHA256SUMS --ignore-missing` (PowerShell:
`Get-FileHash <file>`, compare with the line in `SHA256SUMS`).

**First flash (USB).** Connect the T-2CAN's Type-C. If no new serial port
appears, unplug it, hold **BOOT** while plugging it back in, then release
(download mode). Then either the [ESP web flasher](https://espressif.github.io/esptool-js/) in
Chrome/Edge (connect, the file at offset `0x0`, Program), or esptool, which
needs Python and `pip install esptool`:

```
esptool --chip esp32s3 write-flash 0x0 megaopen-t2can-<ver>-factory.bin
```

That is esptool 5 (what pip installs today). esptool 4 is `esptool.py` and
`write_flash`. `--port COM5` (Windows) or `--port /dev/ttyACM0` picks the
port when it is not found by itself. For `direct` use `--chip esp32` (the
file starts with 4 kB of padding, so it still goes to `0x0`).

**Updates (OTA).** Open `http://<board>/update` and upload `megaopen-<env>-<ver>.bin`,
or:

```
curl "http://<board>/ota/start?mode=fr&hash=$(md5sum fw.bin | cut -d' ' -f1)"
curl -F "file=@fw.bin;filename=firmware.bin" "http://<board>/ota/upload"
```

A new image boots on trial: it confirms itself after 60 s healthy; one that
crashes, or is not healthy within 3 minutes, goes back to the old image (as the
user guide says).

**After an update** press **«Заводські прив'язки»** on the car map page so new
factory signal rows show up (a saved map keeps the old set).

## First boot

1. Join the board's AP `MegaOpen-XXXX` (`XXXX` from the board's MAC address),
   password `megaopen` (or the one in your `src/secrets.h`).
2. Open `http://192.168.4.1`, go to the board page and add your WiFi.
3. On your network the board is at `http://megaopen.local` (or the router's IP).

OTA and the web API have no password of their own; the WiFi/AP password is the
only lock. Change the AP password on the board page.

## Build

You need git, Python 3.11 or newer and [PlatformIO Core](https://platformio.org/)
(`pip install platformio`; CI uses 6.1.19, 6.2.0 also builds). Everything
else is pinned in `platformio.ini`: the first `pio run` downloads the
espressif32 6.13.0 platform, its toolchain and the libraries, about 2.5 GB in
`~/.platformio`, so it takes a while once.

```
git clone https://github.com/Barbedx/MegaOpen.git
cd MegaOpen
pio run -e t2can                 # car build -> .pio/build/t2can/firmware.bin
pio run -e t2can -t factory      # + firmware-factory.bin for 0x0
pio test -e native               # host tests
```

* **Windows: clone into a short path** such as `C:\src\MegaOpen`. Deep in
  `%TEMP%` or a long user folder the compiler fails with
  `xtensa-esp32s3-elf-g++: error: CreateProcess: No such file or directory`
  (the command line outgrows Windows' limit).
* **Host tests** (`native`) use your own C++ compiler, not PlatformIO's:
  `gcc`/`g++` on the PATH (Windows: [w64devkit](https://github.com/skeeto/w64devkit)
  or MSYS2 MinGW; Linux: `build-essential`; macOS: the Xcode command line tools).
* `src/net/gen/` (the gzipped web app) is written by every build and is not in
  git; `tools/check_pages.py` and `tools/check_nav.py` need a build first.
* `warning: refs/tags/v1.1.0 ... is not a commit!` while the AffaDisplay
  library downloads is git's note about an annotated tag; harmless.

Envs: `t2can`, `t2can_usbhid` (USB HID keyboard to the head unit), `direct`,
`native`. Default secrets come from `src/secrets.example.h`, so a clone builds
as it is; copy it to `src/secrets.h` (gitignored) only to change the default AP
password or preset a WiFi network.

## Docs

**Using it:** [docs/USER-GUIDE.md](docs/USER-GUIDE.md): first boot, the panel menu,
the live car page and alerts, media, keys, updates, troubleshooting.

[ARCHITECTURE.md](ARCHITECTURE.md) · [CHANGELOG.md](CHANGELOG.md) ·
[docs/raise-protocol.md](docs/raise-protocol.md) ·
[docs/megane-can-ids.md](docs/megane-can-ids.md) ·
[docs/BACKLOG.md](docs/BACKLOG.md) ·
[docs/RELEASING.md](docs/RELEASING.md)

> Hobby project. It never writes to the car's bus, but you wire it into your own
> car at your own risk. Do not operate the menus while driving.

## License

MIT, see [LICENSE](LICENSE).
