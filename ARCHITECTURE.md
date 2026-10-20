# MegaOpen architecture

This file is an overview. The detailed design notes are
[docs/design/event-tasks.md](docs/design/event-tasks.md) (tasks and concurrency)
and [docs/design/web-ui.md](docs/design/web-ui.md) (the web app and its API).

## Block diagram

```
            ┌──────────── T-2CAN (ESP32-S3) ─────────────┐
 OEM panel ─┤ CAN B  TWAI ── panel task ── AffaDisplay   │
 (Carminat) │         │ keys                ▲ renders    │
            │         ▼                     │            │
 knobs ─I2C─┤ knobs ─► KeyBus ─► keys task ─┤            │── UART 38400 ─► head unit
            │                    │  sinks   ui task      │◄─ media text ── (Raise RAV4)
 iPhone ─BLE┤ AMS/ANCS/CTS ──────┼──────────►│           │
            │                    ▼                       │
 car ─CAN A─┤ MCP2518FD ─► car task ─► decode ─► RAV4 telemetry (lower priority than keys)
 (listen)   │                 └─► sniffer, capture ──► web app / OTA (WiFi)
            └────────────────────────────────────────────┘
```

## Tasks

Priorities, cores and stacks are all in one table, `src/app/TaskPlan.h`. Keys
have the highest priority on core 1, so a key press never waits for a repaint.

| task | prio | core | job |
| --- | --- | --- | --- |
| keys | 7 | 1 | one key dispatcher: KeyBus → router → sinks, macros |
| hu | 6 | 1 | head-unit UART: RX, acks, non-key frames |
| knobs / scan | 6 | 1 | 1 kHz encoder / matrix sampling, posts edges to the KeyBus |
| loop (panel) | 5 | 1 | owns AffaDisplay's `poll()` and the panel's TWAI link |
| car | 4 | 0 | car frames: decode, sniffer, capture, telemetry |
| ui | 2 | 1 | what goes on the glass and when; renders through `PanelOps` |
| httpd, ble | 2 | 0 | web server, NimBLE |
| net_svc, logbus | 1 | 0 | WiFi, housekeeping, log sinks |

The rules behind this layout (the ownership and blocking rules are checked by
`tools/check_rules.py`):

* **One owner per resource.** The panel task owns AffaDisplay's `poll()` and
  the panel's TWAI link (`affa::TwaiLink`); `poll()` from any other task is a
  counted no-op. Any task may render: the library copies a render made off the
  owner into its dispatch ring and admits it inside `poll()`, after the keys.
  `display/PanelOps` is that render surface plus the glass policy (deliberate
  screens, repaints that yield to them).
* **Configs edited from the web are staged.** httpd writes a staged copy, and
  the owner task picks it up (`utils/Staged.h`). Readers get snapshots
  (`utils/Snapshot.h`, `utils/Mirror.h`).
* **UART lanes.** Key frames pre-empt telemetry on the head-unit UART
  (`sinks/UartLanes.h`).
* **Nothing blocks on flash in a hot path.** The flash log and CAN captures are
  written in 16 kB bursts, never on a timer.

## Source map

| dir | what |
| --- | --- |
| `app/` | tasks (`*Task`), the KeyBus, the panel owner, request/reply slots, the OTA trial |
| `board/` | `BoardProfile.h`, the only file that names a pin |
| `bus/` | the MCP2518FD driver, the vehicle-frame sources, autobaud |
| `display/` | AffaDisplay ports (clock, log, panel factory), `PanelOps`, screen/pane composers |
| `keys/`, `input/` | key model, map, router, macros; encoders, debounce, matrix |
| `sinks/` | key outputs: Raise UART, resistive ladder, BLE HID/AMS |
| `media/` | now-playing sources (iPhone AMS, head unit) and the router between them |
| `proto/canbox/` | Raise/canbox framing and the RAV4 profile |
| `vehicle/` | CAN decode table, sniffer, signal registry, vehicle state |
| `capture/` | CAN capture to littlefs (`tools/can_dump.py` reads it) |
| `knobs/` | encoders, LED rings, config |
| `net/` | WiFi, web server and API, `/api/catalog` |
| `cfg/` | NVS-backed settings |
| `utils/` | LogBus, flash log, locks, snapshots |
| `ble/`, `*_service.cpp` | NimBLE hub, HID role, Apple AMS/ANCS and CTS clients |

`main.cpp` wires everything together.

## Feature flags

The `FEATURE_*` switches in `src/Features.h` are set per env in `platformio.ini`.
Code behind a disabled flag is compiled out, not merely switched off. The TWAI
serves the panel where the build has one, else the car (`env:direct`,
listen-only, the link alone through `<AffaTwai.h>`); either way it runs on
AffaDisplay's `TwaiLink`. Runtime
choices (BLE mode, knobs on or off, key routes) are NVS settings that the web
app edits.

## Display

Everything about the AFFA protocol (sync, registration, ISO-TP, the ACK state
machine, key decode, screens) lives in
[AffaDisplay](https://github.com/Barbedx/AffaDisplay), pinned in
`platformio.ini`, and so does the panel's CAN link (`affa::TwaiLink`: the TWAI
driver, the TX gate, bus-off recovery). This repository holds only the ports
and the policy for what gets drawn. The registration sequence as this firmware relies on it is written
down in [docs/DISPLAY-INIT-SPEC.md](docs/DISPLAY-INIT-SPEC.md).

## Head unit

The head unit gets Raise/canbox frames in the Toyota RAV4 profile:
[docs/raise-protocol.md](docs/raise-protocol.md). What the head unit's own
decoder (`com.syu.ms`) actually parses is in [docs/hu-decoder.md](docs/hu-decoder.md).

## Car

CAN A is listen-only, enforced in the controller itself (MCP2518FD ListenOnly
mode). The decode table maps CAN ids to RAV4 fields. It is editable from the web
app (`/api/map`), and its factory defaults are in `vehicle/decode_defaults.inc`.
Known and suspected ids are in [docs/megane-can-ids.md](docs/megane-can-ids.md).
