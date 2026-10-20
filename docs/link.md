# The link: board V (the C3 on the car's CAN) → the brain (the DevKit)

Reference for `src/link/*`, `src/LinkMain.cpp`, `env:boardv` and `/api/link`.
Written 2026-09-22; **nothing here has been on a car or on a second board yet** —
the protocol is host-tested (`test/test_link`, 10 cases), both images build, the
brain end is flashed and reports `alive: false` until a board V answers.

## 1. What crosses the wire

**Raw CAN frames, filtered on board V, never decoded there.** This is deliberate: the
Megane decode does not exist yet — `megane-can-ids.md` is a table of hypotheses —
and the instrument that turns them into facts (`CanSniffer`, the "mark baseline,
open the door, see what changed" page) lives on the brain. So V carries no
knowledge of the car. It carries frames, and a filter the brain gives it.

Binary, 921600 8N1, `0x5A type len payload crc8`. Types: `CAN` (id, dlc, data),
`STATUS` (V → brain, once a second: frames, forwarded, fps, rxErr, busErr,
controller state), `HELLO` (either way on boot), `FILTER` (brain → V). A lost byte
costs one record; the decoder resyncs on the next `0x5A`. A busy car bus at
1500 frames/s is ~20 kB/s against ~90 kB/s of link.

Frames arrive on the brain tagged `Frame::SRC_VH_CAN` and go through `busRx()` —
the same path the brain's own controller uses in the vehicle role — so the
sniffer table, the raw stream and the last-frames ring show them without knowing
which board heard them.

## 2. The filter

Set from `/vehicle` (the "плата V" row) or `POST /api/link/filter`
`{"mode": ..., "ids": ["0x181", "0x60D"]}`. Persisted on the brain, pushed to V on
every boot of either end; V keeps nothing (no NVS, by design).

| mode | forwards |
| --- | --- |
| `all` | every frame — **for the first evening: every id onto the sniffer's table** |
| `changes` | a frame only when its payload differs from the last one with that id, plus one per id per second regardless |
| `list` | only the listed ids (up to 7), every frame |
| `list-changes` | only the listed ids, only when they change |

`changes` is what mapping wants once the ids are known: a door frame arrives when
the door moves, not 20 times a second. The table on V holds 96 ids; an id past
that is forwarded on every frame rather than dropped (traffic, not silence, and
`/api/link` cannot see it yet — watch V's serial `tracked=` line).

## 3. Wiring

```
board V (C3)                      brain (DevKit V1)
GPIO21 (its TX) ───────────────►  GPIO26 (D26)
GPIO20 (its RX) ◄───────────────  GPIO27 (D27)
GND    ────────────────────────   GND
5V     ◄────────────────────────  VIN (5 V)         four wires, one cable

board V (C3)                      car
GPIO3  <-- CRX  ┐ SN65HVD230      OBD2 pin 6  = CAN-H
GPIO4  --> CTX  ┘ (never driven)  OBD2 pin 14 = CAN-L
```

* **Take the 120 Ω off V's transceiver module** before it touches a car. The car's
  bus is terminated at both ends; a third resistor drags it to 40 Ω.
* **Power V from the brain's 5 V, not from OBD2 pin 16** — that pin is live with
  the ignition off, and V would drain the battery.
* **Listen-only at the controller** (`TWAI_MODE_LISTEN_ONLY`, `setTxEnabled(false)`):
  V cannot ACK or transmit on the car bus whatever it is told over the link.

## 4. Flashing and first light

```
pio run -e boardv -t upload            # the C3, over its USB
pio device monitor -e boardv           # "[boardV] can=up ... link=GPIO21->brain GPIO20<-brain @921600"
```

Then on the brain: `GET /api/link` → `alive: true`, `boardV.fps` = what V sees on
the bus, `fps` = what reached the brain, `resyncs` = dropped bytes (should stay 0).
The sniffer table (Авто screen, `GET /api/can/ids`) fills with the car's ids;
the link itself has no screen since ui-v2 slice 8, only `/api/link`. From there the mapping workflow is the existing one:
«Зафіксувати базу», do something in the car, read the `changed` mask.

## 5. Not done

* Decoding: nothing on the brain turns a `SRC_VH_CAN` frame into a `Vehicle::State`
  signal yet. That is the mapping evening's output, and `Signals.h` is where it
  lands.
* V's serial console is the only place its own counters (dropped, tracked) show.
* The C3 as the BRAIN has no link pins (`PIN_LINK_* = -1`); the link is DevKit-only.
