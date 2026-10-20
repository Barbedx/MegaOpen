# Signal glossary: what it is, and what was found on the way

Written 2026-10-17. Nothing here has been measured on the car yet: every row is
a judgement from definitions and documents, and a capture is still owed.

## The three files

| file | what | edited by |
|---|---|---|
| `docs/signal-glossary.json` | For each `Vehicle::Sig` (src/vehicle/Signals.h): where it most likely comes from on OUR Mégane II, the `pick` to make it a default, confidence, evidence. It is meant to seed the firmware defaults later. | hand, plus `glossary --fill` |
| `docs/signal-glossary.md` | The same as a readable table. | **generated**, never by hand |
| `tools/can_corr.py` | The correlator: diagnostic replies in a capture as ground truth for the broadcast frames. | hand |

`pick` is one of:

- `broadcast`: decode a frame the car sends anyway. This is the `decode_defaults.inc`
  table, every row still `DF_HYPOTHESIS`.
- `diag`: poll a read (`21xx`) on the car bus. MegaOpen does not do this yet; see the
  plan order below.
- `derived`: computed in MegaOpen (trip average, range, elapsed time).

```
py -3.12 tools/can_corr.py glossary --fill --ddt <x84 folder>  # encodings from the DDT JSON into ours
py -3.12 tools/can_corr.py glossary --md docs/signal-glossary.md
py -3.12 tools/can_corr.py corr 0007.bin 0008.bin              # a capture -> candidates + decode rows
py -3.12 tools/can_corr.py selftest
```

### Where the data comes from

- **The DDT2000 definitions** (the DDT4all database, `ecu.zip`) are Renault's data, so
  they are not in this repository: unpack the `X84` ECU files (`*.json`) into a folder
  of your own and pass it with `--ddt` (the default is `../ddt-db/x84`). `X84` is the
  Mégane II project code.
- **Which file is OUR car's** came from the owner's DDT4all scan of 2026-06-14
  (its log is not in this repository). The injection is `S3000_A791_Can_1_X84`, the climate
  is `Climatisation_regulee_Redesign_P0_X84_SERIE`, and the UCH is `UCH_84_J84_05_00`.
  Every ECU is listed in the JSON's `ecus`. The S3000 replies in that log decode sanely
  with A791 (idle 747 rpm against a 752 target, 78.75 °C, 13.75 V), so the match is
  believed.
- **The broadcast rows** are still the Clio III decode from x0r.fr/blog/39
  (`docs/megane-can-ids.md`).

The definitions describe DIAGNOSTICS only (KWP2000 on CAN: request, reply, every
field's encoding). They do not list the periodic broadcast frames. Their value for the
broadcast side is indirect, in two ways:

1. Several ECUs report what they RECEIVE over CAN ("... (CAN)", "Recopie ...", the
   climate unit's "Id Producteur ..."). These give the encoding and the sender of a
   broadcast signal, though not its id.
2. A capture taken while DDT4all polls the car has the ground truth and the broadcast
   side by side. `can_corr.py corr` matches the two.

## Findings

### Corroborated: x0r's encodings agree with what the ECUs receive

These are corroborated by the definitions, not measured. They raise the confidence of
the rows; the ids themselves are still x0r's.

| row | evidence |
|---|---|
| `0x181` rpm, be16 /8 | The cluster's "Régime moteur (tr/mn) CAN" is 16-bit /8. |
| `0x354` speed, be16 ×0.01 km/h | The cluster's "Vitesse véhicule (km/h) CAN" is 16-bit /100. |
| `0x551` b0 coolant, u8 −40 | The climate unit's received "Température d'eau" is u8 −40. The ECM's own diag value is 0.625/LSB, an internal scale that is NOT what goes on the bus. |
| `0x551` b1 fuel used, 12 500 = 1 L | The cluster's "carburant consommé (mm3) CAN" is ×80 mm³: 1 L / 80 mm³ = 12 500. |
| `0x60D` b4 outside temp, u8 −40 | The climate unit's received copy is u8 −40. |

### Conflicts and doubts, to settle from a capture

- **Steering bias:** ESP's diag angle is `(raw − 32767)/10`, and the row uses `0x8000`.
  It may be one LSB off, 0.1°.
- **Backlight scale:** the climate unit receives the night-lighting level at 0.4 %/LSB
  (0..250 = 100 %). That does not fit x0r's `0x645` b1 range of 0..0xFE with 0xFF = off.
- **Handbrake (`0x5C5`):** the bit is still in conflict (bit3 vs bit2), and the row is
  left out of decode_defaults. The glossary picks the cluster's own switch read
  (`2110` "Contact frein de parking") as the source, and it is the reference the
  correlator settles the bit against.
- **Brake pedal:** x0r names two places, `0x181` b5 bit0 and the last byte of `0x354`
  bit4. ESP `2106` is the reference. Its value 3 means fault, not pressed.
- **Climate on the bus at all?** The Mégane II panel may talk to the HVAC unit directly.
  If no broadcast byte moves when the fan, recirculation or AC changes, those signals
  are `diag` only (`21C2` on 744).
- **RAV4 driver temperature:** the climate unit has only the BLOWN-AIR setpoint (`21C2`
  b16), not the temperature the driver chose. The blend flap position is the nearest
  proxy. `SIG_AC_TEMP_DRV` has no real source.
- **TPMS:** UCH `2166` holds four wheel pressures (mbar = raw × 3500/255), but only if
  the car has TPMS sensors, and which wheel is "roue 1" is unknown.

### Bit numbering: a trap that was fixed

A DDT2000 file carries an ECU-level `endian`, and an item without its own takes it.
DDT4all (`core/ecu/ecu_data.py`, `getHexValue`) then reads:

- **Big:** `bitoffset` counts from the byte's MSB, and the field runs MSB-first.
- **Little:** inside one byte, `bitoffset` counts from the **LSB**.

OUR UCH file is `Little`, and 1064 of its items set no endian of their own. So the UCH's
door, indicator, reverse and light bits count from the LSB. The first `decode.js`
counted them from the MSB, which put every one of those bits in the wrong place. Now
`decode.js`, `gen_x84.js` (marked "LE" in the tables) and `can_corr.py` (checked by
selftest) all follow DDT4all's rule. The S3000 is also `Little` but sets `Big` on every
item; the cluster, ESP and climate are `Big`.

In DDT4all itself, the little-endian path for a field WIDER than one byte looks wrong:
for a 16-bit field at bit 0 it takes the first byte twice. So DDT4all's on-screen
values for wide unflagged UCH items, such as "Température extérieure (16 bits)", may
be wrong. This is suspected, not tested. `can_corr.py` byte-swaps those fields
instead.

### Not about the project: the car

The same 2026-06-14 log holds a ReadDTC reply with six faults. Two wastegate faults
(open circuit, short to ground) and an alternator "Network anomaly" were PRESENT then,
and boost pressure, the throttle and another wastegate fault were stored. The reply is
not labelled with the ECU it came from, but it decodes coherently only with the S3000
definition. Check whether those faults have been fixed or cleared since.

## Order: the simplest available first

1. **Listen only, no new tools.** Take a capture with `can_dump.py between` and markers
   (door, lights, indicators, reverse, handbrake). This confirms or kills the x0r rows
   that need nothing but the car, with no transmitting.
2. **DDT4all + ELM327 while MegaOpen records in full mode.** Still no firmware change.
   Then run `can_corr.py corr`. It settles the conflicts above, finds the ids of the
   corroborated signals, and shows whether the climate is on the bus at all.
3. **Polling in the firmware.** Only for what step 2 shows is not broadcast (the
   climate, most likely, plus trip1). It ends listen-only on the car bus and needs its
   own plan in `docs/`. Reads only: `21xx` and `17FF00`, never `3B`/`30`/`31`/`11`
   (the UCH has EEPROM-reset and factory-mode writes).
