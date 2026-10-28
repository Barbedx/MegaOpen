# One mapping screen for any head-unit protocol

Status: **plan**, 2026-10-28. Owner: "the mapping screen must be universal, so
the head-unit protocol can be swapped easily"; agreed to start before the
review.

## Today

- **The car's signals** (`Vehicle::Sig`, Signals.cpp) and **the RAV4 fields** are
  the same enum: `SIG_AC_TEMP_DRV`, `SIG_FUEL_INST`, `SIG_RADAR_RL` ... exist only
  because Raise has a field for them. `RaiseTargets` is a list of Sigs, and
  `Rav4Emitter` reads `State` directly (146 `s.get` / `s.has`).
- So "map a head-unit field" edits **the car signal's CAN binding**
  (DecodeTable): the HU row and the car value are one thing. A field cannot take
  another signal (avg speed from the board vs from CAN only works through the
  `Derived::setBound` exception), and the experiments (`exp_7d04`, battery in
  7D 04, computed fuel) are flags that reroute signals by hand.
- Two screens draw it: «Авто — наживо» (signals) and the RAV4 map on «Авто»
  (fields), with different rows, sources and editors.

## The model: three layers

```
 CAN frames ──► Car signals ──► Protocol fields ──► head unit
 (decode table)  (State)         (field table)       (emitter of the active protocol)
                 + computed by the board (Derived)
                 + constants
```

1. **Car signals** describe the car only: speed, rpm, fuel litres, cabin temp,
   doors, lights, gear, consumption, avg speed... Sources: a CAN row (✓ measured
   or hypothesis), a constant, or the board's computation. No Raise names. The
   Raise-only Sigs become fields (below) or are deleted when nothing feeds them.

2. **Protocol fields** belong to the protocol module: `Raise112::fields()` returns
   rows `{id, name key, unit, range, decimals, message, default signal}`. The
   field table (NVS, per protocol: `hu.<proto>` blob) maps each field to a
   **signal id, a constant, or off**, plus an optional **scale** (mul/div/add)
   where the field's unit is not the signal's (litres -> %, km/h x100 -> km/h).
   Defaults come from the module; a saved table keeps the owner's choices.

3. **The emitter** of the active protocol reads `FieldValues` (field id -> value,
   has), filled from State through the field table each tick. It never reads
   State, so it is protocol code only. Today's experiments become plain mapping
   choices (the 7D 04 percent field ← `fuel_level` with scale x100/60; the
   battery in 7D 04 ← `batt_v` in that field) and their flags go.

A protocol module is then: a field list, the message builders, the receive
side (keys, clock, now-playing). Adding another head-unit protocol = a new
module; the screen, the signals and the table code stay.

## The screen («Мапа»)

One page, two tables, same row shape:

| Car signals | value now | source | ✓ |
| --- | --- | --- | --- |
| speed | 62.1 km/h | CAN 0x354 b0-1 | ✓ |
| avg speed | 44.4 km/h | board | — |

| `<protocol>` field | value sent | ← from | test |
| --- | --- | --- | --- |
| avg speed (0x21) | 44.4 | signal avg_speed ▾ | ▶ |
| tank % (7D 04) | 51.6 % | signal fuel_level ▾ x100/60 | ▶ |

- The field's «from» is a list of signals (grouped, with their live values), or
  a constant, or off. The editor for a signal's CAN source lives only in the
  signals table.
- The protocol is chosen in one select at the top (Raise 112 today); the field
  table under it is that protocol's.
- «Авто — наживо» becomes the signals half of this page (its alert rules stay
  there); the RAV4 card on «Авто» goes.

## Data and API

- `GET /api/hu/fields` → the active protocol's fields with their mapping and the
  value sent; `POST /api/hu/fields {rows:[{id, from:{sig|k|off}, scale}]}`.
- `GET /api/map?live=1` stays the signals' feed; `/api/map` loses the Raise rows.
- The catalog serves `protocols: [{id, name}]` and the active one's field list
  instead of `raise`.
- Migration: on the first boot with the field table, each Raise field takes its
  default signal; where the old DecodeTable bound a Raise-only Sig to CAN, that
  row moves to the matching car signal (or a new one) so nothing the owner bound
  is lost. A one-time `[hu] fields migrated: N` line says what moved.

## Slices

1. **Field table under the current emitter:** `FieldValues` filled from State
   with today's identity mapping; Rav4Emitter reads it (146 reads replaced).
   No behaviour change; host tests prove the bytes are identical.
2. **The mapping:** NVS table, defaults from the module, `/api/hu/fields`;
   the experiments become default mappings and their flags are removed.
   *As built (1.6.2 / 1.6.3):* `FieldMap` rules (another signal ×÷+, a
   constant, off, own) keyed by the field's default signal; `FieldView` puts
   them between State and the emitter; NVS `huFmap`, settings file `hu.fields`.
   The module's defaults are rules too (`Rav4Emitter::defaults()`): coolant,
   the PRND probe and the tank % (a new derived signal, `fuel_pct`) start off.
   The experiment bits are gone: `experimentsToRules` turns an old NVS
   `huExp` or a settings file's `exp` into rules once. «Computed» is now a
   plain rule (range ← range_calc and so on) that replaces the field's own
   signal instead of standing in for it when unbound.
3. **Signals cleanup:** Raise-only Sigs become fields; the decode table and
   `Derived` keep car meanings only; the migration.
4. **The page:** the two-table «Мапа», the protocol select, the old RAV4 card
   and the `calc` / `setBound` special cases deleted.
5. **Second protocol stub** (no wire format, a test module) to prove a swap needs
   no change outside the module.
   *As built (5a, 2026-10-28):* `Canbox::FieldProtocol` (src/proto/canbox/Protocol.h):
   `id()`, the field list (`fieldCount`, `field`), `defaults()`, and the
   generic `findField` / `ruleFor` / `withDefaults`. `activeProtocol()` is
   the one swap point (Raise 112 today, from RaiseTargets and
   `Rav4Emitter::defaults`). `/api/hu/fields`, `/api/map`'s field rows and
   the catalog (`proto`, and the field list still under the key `raise`,
   which the Companion app's bundled page reads) go through it; the page
   shows «Протокол ГУ». test/test_protocol runs the map on a stub protocol.
   Still RAV4-bound: the hu task holds a `Rav4Emitter` and its Raise
   `Tuning` (units, 0x28 form) and message table; an emitter interface is
   the next step when a second wire format is chosen.

Each slice: host tests, the RAV4 bytes compared against recorded frames, the
bench, then the car.
