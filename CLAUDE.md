# MegaOpen — agent guide

Firmware for a LilyGO **T-2CAN** (ESP32-S3) in a Renault Mégane II CC. It drives
the OEM AFFA display panel (Carminat), emulates a Raise/RAV4 canbox towards an
Android head unit over UART, talks to an iPhone over BLE, and listens to the
car's CAN. Read `ARCHITECTURE.md` for the block diagram, task table and source
map; this file is the "where do I look" index for a session in the car.

## Rules for a session

- **Diagnose first, report, then ask.** A finding is not a licence to edit
  source. Show the evidence (log lines, `/api/health` codes, file:line) and
  stop unless the owner asks for a fix.
- **CAN A (the car) is listen-only, except the climate read.** The owner's opt-in
  (2026-10-25, Авто › «Діагностика клімату», off by default): `src/app/CarDiag.*`
  sends `21 C1` to the climate ECU (0x744) through `src/diag/ClimDiag.*`. Its
  allowlist (owner, 2026-10-26) also takes the reads `21 80/C0/C2/90/A0` and
  `10 C0` / `3E 01` / `10 81`, for the one-shot sweep and the opt-in session
  (`diag.session`, off by default). Flow control only in answer to a reply. No
  other ECU, no functional 0x7DF, no write (3B), clear, routine, reset or IO
  control. Never add another transmit path; `tools/check_rules.py` fails on one.
- **Never force-push** `master`. Commits use the repo-local identity
  (`www.andruxa@gmail.com`); check `git log -1 --format='%ae %ce'` before a push.
- **Opening the USB serial port can reset the board.** Prefer the web log
  (`/api/logs`) over the COM port.
- **Every image keeps OTA.** Never flash a build without the web server/OTA.
- `src/secrets.h` is gitignored (template: `src/secrets.example.h`). Never
  commit WiFi passwords.

## Reaching the board

| where | address |
| --- | --- |
| car (board's own AP `MegaOpen-XXXX`, pass `megaopen`) | `http://192.168.4.1` |
| home WiFi | `http://megaopen.local` (mDNS) or the router's IP |

## First look when something is wrong

0. After a drive, grab everything at once: `tools/car_report.sh http://<board>`
   (status, health, settings, map, both boots' logs, a summary.txt; GETs only).
1. `GET /api/health` — one object per chain with `state` and `code` (the words
   live in `web/i18n/*.json` as `w.<code>`; error replies are codes too, `e.<code>`):
   `panel`, `media`, `clock`, `keys` (+ `dest` masks), `ancs`, `hu`, `knobs`,
   `lights`, `phone`. Start from the `code`.
2. `GET /api/status` — build hash, uptime, heap, RSSI.
3. `GET /api/logs?since=0` — live log ring; `GET /api/logs/file` — the flash
   log (survives reboots, written on warnings and before a reboot).
4. Periodic lines every 10 s tell the story without a debugger:

| tag | what |
| --- | --- |
| `[hb]` | uptime, heap, min heap, CAN rx, keys |
| `[can]`, `AFFA-TWAI` | panel bus: state, tec/rec, bus errors, TX gate ("nobody ACKs"), reinstalls |
| `[panel]`, `[disp]`, `AFFA` | registration phase, posted/delivered renders, failStreak |
| `[key]` | key latency p50/p99, drops, source counts |
| `[hu]` | head-unit UART: acks, replies, desync, drops |
| `[car]`, `[can2]` | car CAN (MCP2518FD): fps, autobaud, overflows |
| `[ui]`, `[media]` | glass policy, now-playing source |
| `[ble]` | iPhone link, AMS/HID queues |
| `[ota]` | trial / CONFIRMED / rolling back |
| `[push]` | log push to GitHub: file sent, bytes queued, GitHub's refusal |
| `[alert]` | panel alert rules |
| `[mem]`, `[win]`, `[stk]`, `[task]`, `[stall]` | heap, stacks, task ages, loop gaps |

Raise the log level for one session through `/api/settings` `{"logLevel":"DBG"}`
(volatile; reboot returns to INF). `TRC` shows panel frames and slows the loop.

## Symptom → where to look

| symptom | API / log | code |
| --- | --- | --- |
| panel blank, never registers | health `panel`, `[disp]`, `AFFA-TWAI` | `src/app/PanelOwner.*`, `src/display/AffaPort.cpp`; protocol in AffaDisplay (`.pio/libdeps/t2can/AffaDisplay`), sequence in `docs/DISPLAY-INIT-SPEC.md` |
| panel keys slow / lost | `[key]` p99, drops | `src/keys/AffaKeySource.*`, `src/app/KeysTask.*`, `src/keys/KeyRouter.*` |
| wrong text / layout on glass | `[ui]`, `[media]` | `src/app/UiTask.*`, `src/display/PanelOps.*`, `MediaScreen.*`, `MediaPane.*`, `GlassGate.h` |
| panel menu (hold right knob) | — | `src/app/PanelMenu.*`, `src/ui/MenuEngine.*`, `MenuInput.*` |
| car screen (3 signals) | — | `src/ui/SignalText.*`, `src/app/UiTask.*` |
| encoders / LED rings | health `knobs`, `lights`; `/api/knobs` | `src/knobs/*`, `src/input/*` |
| head unit shows nothing / wrong | health `hu`; `/api/canbox/last?in=1`; `/api/messages` | `src/app/HuTask.*`, `src/proto/canbox/Rav4Emitter.*`, `src/sinks/CanboxUartSink.*`; `docs/raise-protocol.md`, `docs/hu-decoder.md` |
| track title from HU | `[media]` | `src/media/HuMediaSource.*`, `MediaRouter.*` |
| iPhone: no track / clock / notifications | health `phone`, `media`, `clock`, `ancs`; `[ble]` | `src/ble/*`, `src/apple_*_service.cpp`, `src/current_time_service.cpp`, `src/media/AmsMediaSource.*` |
| car value wrong / missing | «Мапа» page `#map` (`/api/map?live=1`), `[car]`, `[can2]` | `src/vehicle/decode_defaults.inc` (factory rows), `CanDecode.*`, `Signals.*`, `Derived.*` (gear, consumption, range); ids in `docs/megane-can-ids.md`, `docs/signal-glossary.md`; car checklist `docs/car-test-can-signals.md` |
| new signals not shown | saved map hides new factory rows | press **«Заводські прив'язки»** on «Мапа» › Сигнали |
| alert popup wrong / spam | `[alert]`; rules in `/api/settings` `alerts` | `src/vehicle/AlertRules.*` |
| car CAN silent | `[can2]` autobaud, `ab=searching` | `src/bus/Mcp2518Source.*`, `Autobaud.*`, `src/app/CarTask.*` |
| a setting does not apply / save | `POST /api/settings` answers 400 with key + reason | `src/cfg/SettingsRegistry.cpp` (every setting, its range and apply bit), `Config.cpp` (NVS) |
| web page broken | browser console | `web/js/*.js` (built into the image by `tools/gen_pages.py`), `src/net/WebUi*.cpp` |
| OTA rolls back | `[ota]` | `src/app/OtaTrial.*`, `OtaVerdict.*` (confirm after 60 s healthy, revert at 180 s) |
| reboots / heap low | `[hb]`, `[mem]`, flash log | `src/app/Housekeeping.*`, `TaskPlan.h` (priorities, stacks) |
| WiFi / AP | `/api/wifi` | `src/net/WifiManager.*` |
| remote log not arriving | `/api/logpush` (`state`, `err`), `[push]` | `src/net/LogPush.*`, `TlsJob.*`; the files: `gh api repos/Barbedx/megaopen-logs/contents/mo-XXXX` or clone the repo |
| climate read (cabin temp) | `/api/diag` (`state`, `last`, `reply`), `[diag]`, `W [can2] diagnostic read stopped` | `src/app/CarDiag.*`, `src/diag/ClimDiag.*`, `src/bus/Mcp2518Source.*` (`setTransmit`, `send`) |

Pins live only in `src/board/BoardProfile.h`. Feature flags: `src/Features.h`
plus per-env `-D` in `platformio.ini`.

## Build, test, flash

PlatformIO may not be on PATH: `~/.platformio/penv/Scripts/pio.exe`.

| what | command |
| --- | --- |
| car build | `pio run -e t2can` → `.pio/build/t2can/firmware.bin` |
| other envs | `t2can_usbhid` (USB keyboard), `direct` (DevKit, no panel) |
| host tests | `pio test -e native` |
| repo checks | `python tools/check_rules.py`, `tools/build_sim.py`, `tools/check_pages.py` |
| USB flash | `pio run -e t2can -t upload --upload-port COMx` |
| OTA | `H=$(md5sum firmware.bin \| cut -d' ' -f1); curl -H "X-MegaOpen: 1" "$B/ota/start?mode=fr&hash=$H"; curl -H "X-MegaOpen: 1" -F "file=@firmware.bin;filename=firmware.bin" "$B/ota/upload"` then wait for `[ota] CONFIRMED`. `$B` must be the IP or `<name>.local` (Host check); every non-GET needs the `X-MegaOpen: 1` header |

## Library

AffaDisplay (`github.com/Barbedx/AffaDisplay`, pinned by release tag, e.g.
`#v1.0.0`, in `platformio.ini`) owns the AFFA protocol and the panel's CAN link
(`affa::TwaiLink`, TX gate, bus-off recovery). Library bugs are fixed there and
released as a new tag; MegaOpen then bumps the pin. Never patch
`.pio/libdeps`. Its known gaps are in its `CHANGELOG.md`.

## Releases

Tag `vX.Y.Z-rcN` → CI publishes a prerelease (OTA `.bin`, `-factory.bin` for
the first USB flash, `SHA256SUMS`). The rc's OTA asset is verified on a real
board (`[ota] CONFIRMED`), then `vX.Y.Z` on the same commit republishes those
exact files as latest. The tag must match `MEGAOPEN_VERSION` in `src/Version.h`.
Full procedure and rules: `docs/RELEASING.md`.

## Docs

`docs/BACKLOG.md` (open work, known bugs), `docs/DISPLAY-INIT-SPEC.md`,
`docs/raise-protocol.md`, `docs/hu-decoder.md`, `docs/megane-can-ids.md`,
`docs/signal-glossary.md`, `docs/car-test-can-signals.md`, `docs/knobs.md`,
`docs/wiring-direct.md`, `docs/design/event-tasks.md`, `docs/design/web-ui.md`,
`hardware/` (wiring drawings, prints).
