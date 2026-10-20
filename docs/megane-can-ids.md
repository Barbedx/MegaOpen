# Mégane II CAN — hypothesis table

> **Superseded in part.** `raise-protocol.md` §6 now carries a researched version of
> this table, split into confirmed / probable / not-transferable / refuted, with the
> vehicle each source actually covers. Read that first; this file is kept for the
> extraction convention below and because it is what the firmware's decode table will
> be shaped like.

**Status: unverified hypotheses, not measurements.** These IDs and layouts come from
the Clio III decode published at `x0r.fr/blog/39` (same platform generation) and were
carried into the previous project's decode table without ever being confirmed against
a running car. Treat every row as a candidate to check, not as fact.

Since Step 5 (2026-10-15) these rows ARE compiled in, as `src/vehicle/decode_defaults.inc`,
every one flagged `DF_HYPOTHESIS`; the car task decodes them into `Vehicle::State`, and the
Raise emitter stays off until they are confirmed on the car. A change here means a change
there. The table still exists so that the sniff/diff workflow starts with somewhere to look
instead of 200 unknown IDs.

Extraction convention: `raw = big-endian(numBytes at startByte)`, then
`field = (raw >> shift) & ((1 << bits) - 1)`, then `value = field * mul / div + add`.

| CAN ID | Byte(s) | Signal | Encoding | Confidence |
| --- | --- | --- | --- | --- |
| `0x0C2` | 0-1 | steering angle | `raw - 0x8000`, 0.1°/LSB | medium |
| `0x181` | 0-1 | engine rpm | `raw / 8` | medium |
| `0x181` | 5 bit0 | brake pedal | 0/1 | low |
| `0x354` | 0-1 | road speed | km/h ×100 | medium |
| `0x551` | 0 | coolant temperature | `raw - 40` °C | medium |
| `0x5C5` | 0 bit3 | handbrake | 0/1 — another source says bit2 | low |
| `0x5FD` | 0-2 | odometer | km | medium |
| `0x60D` | 0-2 | status bit field | see below | medium |
| `0x60D` | 4 | **outdoor temperature** | `raw - 40` °C | medium |
| `0x60D` | 6 bit4 | reverse gear | 0/1 | low |
| `0x645` | 1 | **instrument illumination** | 0x00-0xFE, 0xFF = off | medium |

`0x60D` bits, numbered inside the 24-bit big-endian word of bytes 0-2:

| Bit | Signal | Bit | Signal |
| --- | --- | --- | --- |
| 23 | boot | 13 | indicator left |
| 22 | door rear right | 11 | main beam |
| 21 | door rear left | 10 | key on |
| 20 | door front right | 9 | key ACC |
| 19 | door front left | 8 | front fog |
| 18 | position lights | 2 | rear fog |
| 17 | dipped beam | | |
| 14 | indicator right | | |

## Why this matters for the wiring decision

Two of these rows overlap with signals otherwise available as physical wires on the
display connector (ARCHITECTURE.md §7.2):

- **outdoor temperature** — `0x60D` byte 4. If it confirms, the NTC wire is
  unnecessary, which is the recommendation already recorded.
- **illumination** — `0x645` byte 1. If it confirms, the dimmer wire and its RC
  filter are unnecessary too. This is a *maybe*, not a plan: it is the row most
  worth checking early, because it decides whether that input is ever built.

Indicators need a latch when decoded (they arrive as blink pulses, so a naive
decode toggles the head unit's arrow at 1.5 Hz). The previous implementation used a
500 ms re-armed timestamp latch; the same approach applies here.
