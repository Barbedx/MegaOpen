# Wiring — the direct-input build (`env:direct`)

Board: **ESP32 DevKit V1 (WROOM-32)**. Every pin below comes from
`src/board/BoardProfile.h`, which is the only file in the project that names one
— if something here disagrees with the firmware, that file wins.

> **There is no USB-versus-12 V rule.** This line used to claim the two must
> never be connected together and that a transceiver had been lost to it. The
> owner rejected that outright on 2026-08-12, and the whole of that session was
> flashed over USB on COM5 with the bench powered, repeatedly, with no ill
> effect. Removed rather than softened, because a false warning costs exactly as
> much attention as a true one.

---

## 0. What can be built today, and what cannot

| Path | Parts | Status |
| --- | --- | --- |
| Keypad → ESP32 (6 wires) | wire only | **buildable now** |
| ESP32 → KEY1/KEY2 ladder | 6 resistors | **buildable now** (see §4 on the drive) |
| ESP32 → head unit UART | 2 wires | **buildable now**, but the HU connector pin is unconfirmed — §5 |
| Vehicle CAN → ESP32 | SN65HVD230 | **buildable now.** This row used to read "BLOCKED — you have none, the last one is dead and binned". On 2026-09-20 that module was tested with `19_cantest`'s `op=level` and found perfectly healthy: the fault was an intermittent solder joint on the CRX line. **Modules binned as dead should be re-tested before any are ordered.** For anything going into a car, prefer TJA1051T/3 — the SN65HVD230's bus pins are rated −4…+16 V and it is not an automotive part. |
| 12 V → 5 V | DC-DC step-down (`id 35`) | **buildable now** |

The firmware does not care: the CAN half is compiled in, listen-only, and simply
reports nothing until a transceiver exists. Everything else works without it.

---

## 1. The whole thing

```
   Mégane keypad                ESP32 DevKit V1                 DUDU7
   ┌──────────┐                ┌───────────────┐            ┌──────────────┐
   │ 6 wires  ├───────────────►│ 25/26/27 rows │            │              │
   │ 3x3 + enc│                │ 32/33/14 cols │            │              │
   └──────────┘                │               │            │              │
                               │  13,18,19,    │  ladder    │              │
                               │  23,21  ──────┼──[ R x5 ]──┤ G7   KEY1    │
                               │  22     ──────┼──[ R x1 ]──┤ G6   KEY2    │
                               │               │            │              │
                               │  GPIO17  TX ──┼───────────►│ HU RX  (§5)  │
                               │  GPIO16  RX ◄─┼────────────┤ HU TX  (§5)  │
                               │               │            │              │
                               │  GND     ─────┼────────────┤ G1   GND     │
                               │  VIN  ◄── 5V ─┼── DC-DC ◄──┤ G3   ACC 12V │
                               │               │            │              │
                               │  GPIO5  CRX ◄─┼─┐          └──────────────┘
                               │  GPIO4  CTX ──┼─┤ SN65HVD230 ──► OBD2 6/14
                               └───────────────┘ └   (NOT IN STOCK)
```

---

## 2. Full pin table

| GPIO | Direction | Goes to | Note |
| --- | --- | --- | --- |
| 25, 26, 27 | out / Hi-Z | keypad wires **4, 5, 6** | matrix rows, driven LOW one at a time, never HIGH |
| 32, 33, 14 | in, pull-up | keypad wires **1, 3, 2** | matrix columns |
| 13, 18, 19, 23, 21 | out / Hi-Z | ladder legs → **G7 (KEY1)** | 150, 390, 820, 1800, 3300 Ω |
| 22 | out / Hi-Z | ladder leg → **G6 (KEY2)** | one leg; 1 kΩ is a good value |
| 17 | out | head unit **RX** | UART TX, 38400 8N1, 3.3 V |
| 16 | in | head unit **TX** | UART RX — see §5 before connecting |
| 4 | out | transceiver **CTX** | CAN, when you have one |
| 5 | in | transceiver **CRX** | |
| VIN | — | DC-DC 5 V out | |
| GND | — | **G1** | common ground, mandatory |

**Do not use GPIO 12.** It is MTDI: it must read LOW at reset or the bootloader
picks the wrong flash voltage and the board will not come up — and a ladder leg
is pulled HIGH by the head unit whenever the pin is Hi-Z, which is most of the
time. GPIO 0/1/3 are the console, 6–11 are the flash, 34–39 cannot drive.

---

## 3. Keypad → ESP32 (6 wires)

```
  keypad wire 4 ── GPIO25  ┐                 pos0 VOL-   pos1 MODE   pos2 VOL+
  keypad wire 5 ── GPIO26  ├ rows (drive LOW) pos3 SRC    pos4 RADIO  pos5 MUTE
  keypad wire 6 ── GPIO27  ┘                 pos6 encA   pos7 encB   pos8 encC

  keypad wire 1 ── GPIO32  ┐
  keypad wire 3 ── GPIO33  ├ columns (INPUT_PULLUP)
  keypad wire 2 ── GPIO14  ┘
```

Six buttons plus a **three-phase** wheel — not quadrature. The three bottom
positions close in a repeating cycle and direction is which way round it goes.
If a car turns out different, every position is reassignable on `/` — nothing
here needs re-soldering.

**Automotive protection:** 1–2 kΩ in series on all six lines plus a clamp to rail
and ground. It is a 12 V harness; unprotected GPIO does not last.

---

## 4. The ladder → KEY1 / KEY2

```
  GPIO13 ──[ 150Ω ]──┐
  GPIO18 ──[ 390Ω ]──┤
  GPIO19 ──[ 820Ω ]──┼───────────────►  G7   KEY1
  GPIO23 ──[1800Ω ]──┤
  GPIO21 ──[3300Ω ]──┘

  GPIO22 ──[ 1kΩ  ]──────────────────►  G6   KEY2

     GND ───────────────────────────►  G1   GND      ← mandatory, shared
```

All five are E12 values, so they are in any assortment bag. They are also within
3% of the best 5-resistor set that exists for this job — smaller than the error
from 5% tolerance, so there is nothing to gain by hunting for better ones.

**One leg per button**, not combinations: five legs reach 31 values but a dozen
of them bunch within a few percent at the bottom. Firmware hands out single-leg
rungs first and only uses combinations past the fifth button. Eight buttons come
out ~50% apart, which is comfortable.

### Which drive — MEASURED, and the answer is bare GPIO

**2026-08-12, on this head unit: KEY1/KEY2 idle at 2.8 V, the canbox UART is
3.3 V.** So the resistors go **straight to the GPIO** — no transistors, nothing
else in the path.

Why that measurement is the whole decision: a Hi-Z leg is not disconnected — the
resistor ties the KEY line to the pin. Had the line idled *above* the 3.3 V rail,
the pin's ESD clamp would conduct and pull the idle level down (not a damage
risk — the unit's own pull-up limits it to a fraction of a milliamp — but the
unit could read a button nobody pressed). At **2.8 V the clamp never conducts**,
so a released leg is a genuinely open switch.

In `POST /api/res` (no screen since ui-v2 slice 8): drive = **прямий GPIO**, series = **30 Ω** (the pin's own driver
resistance, 20 % of the 150 Ω bottom rung, so not optional). Both are already the
firmware defaults.

> **The one setting that must match the solder.** Selecting **MOSFET** on a
> bare-GPIO board inverts the logic — released becomes `OUTPUT LOW`, i.e. every
> button stuck pressed. Nothing is damaged and it is immediately obvious, but it
> is the only switch here that can be wrong in a confusing way.

<details><summary>If you ever wire this to a 5 V unit</summary>

Fit one small N-MOSFET per leg — **2N7002 or BSS138**. The IRF3205 in stock is
*not* logic-level (gate threshold runs to 4 V) and will not work from 3.3 V. A
2N3904-class NPN with a 4.7 kΩ base resistor is an equally fine substitute: its
~0.15 V saturation offset is constant, and the head unit learns whatever it is
shown. Then set drive = **MOSFET**, series = **2 Ω**.

```
   MOSFET variant, per leg:            NPN variant, per leg:
   GPIO ──┤ G                          GPIO ──[4k7]──┤ B
       2N7002  D ──[ R ]── KEY               2N3904   C ──[ R ]── KEY
          S ── GND                              E ── GND
```
</details>

---

## 5. UART to the head unit — one pin still unconfirmed

TX and RX are GPIO17 and GPIO16, 38400 8N1, **3.3 V, no level shifter**. Three
independent implementations wire an ESP32 GPIO straight to these units'
canbox lines — see `docs/raise-protocol.md` §1a.

**The DUDU7 carries them on connector [C] "CANBUS/Front Camera"** (8 pins, 2×4,
from the DUDU wiring page): C1 GND, C2 BATT 12 V, C3/C4 AUX 2 L/R, C5 KEY_RST,
C6 TV-IN, **C7 CAN_RXD "CANBUS TX"** (the unit's input, our TX goes here),
**C8 CAN_TXD "CANBUS RX"** (the unit's output, into our RX). Pin 7 is in the
odd row (1 3 5 7), on the latch side. In the owner's harness only C7 and C8
are populated.

On the T-2CAN (2026-10-19), through a QWIIC breakout soldered to the plug's
wires and a stock QWIIC cable into QWIIC 1:

| DUDU7 [C] | harness wire | breakout | QWIIC wire | T-2CAN |
| --- | --- | --- | --- | --- |
| C7 (HU RX) | green | SDA | blue | TX, GPIO43 |
| C8 (HU TX) | black-white | SCL | yellow | RX, GPIO44 |
| C1 or HU ground | — | GND | black | GND |
| — | — | VCC | red | not connected |

The direction is read from the labels (each pin carries both an RXD and a TX
name), not yet proven by traffic: if nothing arrives from the unit, swap C7/C8
first. Never take this ground from the CAN terminals: their GND is isolated.

**MEASURED at 3.3 V on this unit (2026-08-12)**, so wire both directions from the
start — no divider, no series resistor on RX. The TX-only caution that used to
stand here was for the case where the head unit's TX idles at 5 V and meets a pin
that is not 5 V tolerant; that does not apply here. Connecting RX immediately is
worth doing rather than deferring: the `R` and `J` lines in the log are the only
thing that answers whether the unit acknowledges anything at all, or is speaking
a different dialect entirely.

---

## 6. Vehicle CAN — blocked on a part

500 kbit/s, listen-only, OBD2 **pin 6 = CAN-H, pin 14 = CAN-L**, ground on pin 4
or 5. `GPIO4 → CTX`, `GPIO5 ← CRX`.

`setCANPins(rx, tx)` takes **RX first** — the reversed pair fails silently, with
every error counter reading zero. No termination resistor: an OBD tap is not a
two-node stub.

You need an **SN65HVD230** (3.3 V). Order one; the firmware half is done and
tested, and until then the sniffer simply reports nothing.

---

## 7. Power

```
  G2  B+12V ──┐                                (permanent, for later sleep work)
  G3  ACC12V ──► DC-DC step-down ──► 5V ──► ESP32 VIN
  G1  GND ─────────────────────────────────► ESP32 GND
```

ACC rather than B+ so the module dies with the ignition.

**The rule that used to be here is deleted, not softened.** It said the USB and
the 12 V supply must never be connected at the same time — contradicting §0 of
this same file, which had already recorded the owner rejecting it. The real
constraint is about the GROUND, and it is worth stating properly because it is
what actually kills transceivers:

> A CAN receiver decides on the DIFFERENCE between CANH and CANL, but it is a
> comparator referenced to its own supply. The absolute level of both lines must
> stay inside **−2 … +7 V of the transceiver's own GND** (−4 … +16 V absolute).
> Differential does not mean floating.

With two supplies and no solid common ground, nothing constrains where the two
grounds sit relative to each other, and that difference lands straight on
CANH/CANL. In the car this is free — the chassis is the common ground, provided
the ESP32 is powered from the car. On a bench, with USB from a PC and a separate
12 V supply, it is not free and it is the whole risk.

**Ground connects first and disconnects last**, and nothing is re-plugged on the
bus while the 12 V side is live.

---

## 8. Bring-up order

1. Bench, USB only. Flash `pio run -e direct -t upload`. Board comes up as AP
   `MegaOpen-XXXX` / `megaopen`, or on saved WiFi.
2. Keypad only. Open `/` and check every button lights its position, and that the
   wheel counts both directions.
3. Drive is already correct for this unit (bare GPIO, 2.8 V measured — §4).
   **Teach the head unit**
   — read `docs/resistive-keys.md` §5 first: function on screen *first*, hold
   >3 s, save once at the end. The wheel needs the **навчити 4с** button, since a
   detent is 60 ms and can never be held.
4. Enable the resistive sink and tick the classes you want on `/` → routing.
5. UART, both directions (§5 — the line is 3.3 V, confirmed). Watch the `T` lines
   going out and the `R` / `J` lines coming back.
6. CAN last, once a transceiver arrives.
