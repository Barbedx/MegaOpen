# Rules: one event mechanism for alerts, reminders and the head unit (1.7)

Status: **plan, waiting for the owner's answers** (2026-10-28, "Open questions"
at the end). Target: 1.7.0. Owner's ask: "not hardcoded; defaults are rows
that can be edited later; consistent; like Home Assistant automations; the
same mechanism for the UART (head-unit) mapping; one config file, so it works
on different cars."

## Today: four mechanisms that do similar things

| what | where | editable? | shape |
| --- | --- | --- | --- |
| CAN row → signal | `DecodeTable` (NVS `decode`, 96 rows + 16 constants), factory rows in `decode_defaults.inc` | yes, «Мапа › Сигнали» | data |
| signal → head-unit field | `Canbox::FieldMap` (NVS `huFmap`, 32 rules: own / other signal ×÷+ / constant / off) | yes, «Мапа › Поля ГУ» | data |
| alert popup | `AlertRules` (NVS `alerts`, 16 rows: ONE signal, above/below/rises/falls/changes, pause) | yes, «Мапа › Сповіщення» | data, but one condition only |
| reminders "driving without lights", "handbrake while driving" | `Derived.cpp`: C++ sets `drive_no_lights`, `drive_handbrake`; two speeds in Settings › Reminders | only the speed | **hardcoded** |
| gear, consumption, range, avg speed, `fuel_pct` | `Derived.cpp` | gear table only | C++ math (integration, matching): stays C++ |

The signals are a compile-time enum (`Vehicle::Sig`, ~170) mixing car signals,
head-unit-only ones (radar, TPMS, the 0x28 A/C form) and the board's computed
ones; saved tables store the enum index, which already bit us once (1.6.3:
`SIG_COUNT` moved and empty rows became `fuel_pct`).

## The model

```
 sources                       signals (one namespace, by NAME)         consumers
 CAN rows (DecodeTable) ──►  car: speed, handbrake, lights_dipped ...  ──► HU fields (FieldMap: binding, periodic)
 board math (Derived)   ──►  board: gear, cons_inst, fuel_pct ...      ──► rules ──► actions:
 rules (set a signal)   ──►  user: drive_no_lights, parked_dark ...           popup on the panel
 head unit (UART in)    ──►  hu: hu_volume, hu_source ... (later)             set a user signal
 phone (BLE)            ──►  phone: call_active ... (later)                    send an HU message now
                                                                              (later: LED ring, key action, log)
```

Two primitives, both pure data, both edited on «Мапа»:

1. **Binding** = a *state*: "this HU field is always this signal (×÷+)". Today's
   FieldMap, unchanged in spirit. The head unit needs its frames repeated (it
   times out a display that goes quiet), so a field is not an event; the
   protocol module keeps its schedule. New: per message, **«одразу при зміні»**
   (send immediately when any of its fields changes, then keep the period), so
   "when speed changes, send speed" is a switch on the speed message, not a rule.

2. **Rule** = an *event*: `WHEN <conditions> [FOR n s] → DO <actions>`, with a
   pause and an on/off switch. This is the HA automation:

   ```
   «Їду без світла»   WHEN speed > 10 AND lights_dipped = 0 AND lights_pos = 0  FOR 3 s
                      DO  popup "Увімкніть світло"; set drive_no_lights = 1
                      ELSE (condition gone)  set drive_no_lights = 0
   «Ручник»           WHEN handbrake = 1 AND speed > 5  FOR 2 s   DO popup
   «Мало пального»    WHEN fuel_low rises                         DO popup   pause 300 s
   ```

   - **Condition**: `signal  op  value`, op = `>  <  =  ≠  rises  falls  changes
     stale` (stale = no CAN for n s); a value can be another signal
     (`speed > speed_limit`). All of (AND) or any of (OR) per rule; no nesting.
   - **FOR n s**: the conditions must hold that long (debounce, "for 3 seconds").
     Leaving is immediate, or after its own n s (the hysteresis the reminders
     have today: "stops below half its speed" becomes `FOR`/a clear threshold).
   - **Actions**: up to 3. `popup` (text with `{signal}` placeholders, today's
     alert text by default), `set <user signal> = value` (and back on ELSE),
     `hu <message> now`. Later slices: LED ring flash, key action, log line.
   - **User signals**: named values only rules write (bool or int, unit,
     group «Свої»). They show on «Сигнали», can take an alert and **can feed an
     HU field** — that is how a car-specific condition reaches the head unit.

**Defaults are rows**, not code: the factory profile is a JSON document in the
image (decode rows + rules + user signals + HU bindings). «Заводські …» copies
its rows in; the owner edits them like any own row. `drive_no_lights` /
`drive_handbrake` leave `Derived.cpp` and become two factory rules; Settings ›
Reminders goes (its two speeds become the rules' thresholds, migrated once).

**One file per car**: the settings file already carries `decode`, `hu` and
`alerts`; it gains `rules` and `userSigs`, all keyed by signal NAME. A profile
for another car = that file with its own decode rows and rules (the HU
protocol stays a firmware module; see "Not in 1.7").

## Signals, described

Every signal row (`/api/map?meta=1`, the catalog) gets a **source class**, the
grouping on «Сигнали» (filter chips instead of today's car-system groups only):

| class | what | ✓ / ? |
| --- | --- | --- |
| CAN ✓ | bound to CAN, measured in a car | ✓ |
| CAN ? | bound to CAN, Renault's position, not yet checked | ? |
| board | computed by the board (gear, consumption, `fuel_pct`) | as its weakest input |
| user | written by a rule | — |
| const / none | a constant, or nothing feeds it | — |
| HU, phone | (later) values the head unit / iPhone report | — |

The car-system group (Рух, Кузов, Світло …) stays as the second key. Config
refers to a signal by **name** everywhere (rules, alerts, HU bindings, the
file); the enum index stays an in-RAM detail. No more index migrations.

## Efficient on the board

- `State::set` already runs on the car task; it gains a **changed bitmap**
  (one bit per signal, ~24 B). Each rule knows its input signals (a bitmask);
  a tick (10 Hz) evaluates only rules whose inputs changed, plus rules with a
  running `FOR` timer. Idle car = near zero work.
- Fixed tables, no heap churn: 32 rules × 4 conditions × 3 actions, 16 user
  signals; ~2 kB, in PSRAM (8 MB free on the T-2CAN; DRAM heap is the scarce one).
  NVS: one versioned blob `rules` (+ `usig`), names stored as the signal's
  saved id with a NAME table in the blob so a reordered enum cannot shift them.
- Popups keep today's queue rules (one pending, never over a menu, dropped if
  over). Host tests (env:native) cover the engine like `test_decode` does
  `AlertEval` today.

## Slices (each: host tests, sim, bench OTA + rc_smoke, then the car)

1. **Names and source classes.** Every saved table keyed by signal name (the
   alert blob and FieldMap migrate once); `/api/map?meta=1` sends the class;
   «Сигнали» filters by class. No behaviour change.
2. **Rule engine** (`src/vehicle/Rules.*`, portable): conditions, AND/OR, FOR,
   ELSE, actions popup + set; changed-bitmap evaluation; NVS `rules`; settings
   file `rules`. Today's 16 alert rows migrate into rules (1 condition + popup +
   pause); `AlertRules`/`AlertEval` are deleted, `/api/settings alerts` goes.
3. **User signals** (`usig`): rows on «Сигнали» (group «Свої»), usable in rules,
   alerts and HU bindings.
4. **Reminders as rules.** `drive_no_lights` / `drive_handbrake` become user
   signals written by two factory rules; their C++ and Settings › Reminders go.
5. **The editor**: «Мапа › Правила» (today's «Сповіщення» tab, renamed): a rule
   is a card — name, WHEN rows (signal ▾ op ▾ value), FOR, DO rows, pause, on;
   «Додати заводські правила». Live state on each card (holds / waiting n s /
   fired 12:03). i18n keys, ui_audit at phone width.
6. **Head unit**: «одразу при зміні» per message (protocol schedule + on-change),
   the `hu <message> now` action, user signals in «Поля ГУ».
7. **Profile file**: the factory profile as JSON in the image
   (`decode_defaults.inc` stays the CAN rows' source, generated into it), export
   / import of the whole car config, docs (USER-GUIDE «Правила», CHANGELOG).

## Not in 1.7

- **HU protocol as data** (frame templates: header, bytes from fields, checksum)
  so another canbox protocol needs no firmware: possible, but every protocol's
  head-unit decoder is its own; one more protocol module first (hu-mapping
  step 5b) shows what a template must express. Candidate for 1.8.
- Expressions/math in rules (`speed * 2 > rpm`): bindings already scale ×÷+;
  rules compare only. Board math stays in `Derived`.
- Rule editing from the panel menu (on/off per rule could come cheap).

## Decisions (owner, 2026-10-28)

These override the sections above where they disagree.

1. **No small fixed tables.** NVS is 20 KB and already holds other things.
   Rules, user signals, the decode table and the HU field maps live in one
   file in LittleFS (`/cfg/car.json`; the 9.9 MB partition that already holds
   the flash log and CAN captures) and in PSRAM as dynamic arrays, with only a
   sanity cap (256 rules, 64 user signals, condition depth 4). NVS keeps the
   small board settings; `decode` / `huFmap` / `alerts` migrate out of it once.
2. **Nested conditions**: an AND / OR / NOT tree, HA-like, depth ≤ 4.
3. **Factory rules**: low fuel, driving without lights, handbrake while driving.
4. **«одразу при зміні»** is on for every HU message, with a minimum gap per
   message (100 ms) so UART traffic stays bounded.
5. **HU and phone signals are in 1.7**: values coming back from the head unit
   (UART: volume, source, its keys, clock) and the iPhone (BLE: call, track)
   are signals in the same namespace, so rules can use them.
6. **Settings › Reminders and `/api/settings alerts` go** (migrated, no
   alias); the tab «Сповіщення» becomes «Правила».
7. **The HU protocol is a module behind an interface** (slice 0), picked by the
   setting `hu.protocol`; Raise/RAV4 is its first implementation. A protocol
   described as data (frame templates) stays out of 1.7.
8. **Field maps are kept per protocol**: switching protocol does not lose the
   other one's bindings (a few hundred bytes each in the file).

## Slice 0: the head-unit protocol as a module

Today Raise/RAV4 leaks out of `src/proto/canbox/`: `HuTask` owns a
`Rav4Emitter`, `main.cpp` holds the global, `Config` / `SettingsRegistry`
size the message switches by `Rav4Emitter::MAX_MESSAGES` and own `raise.*`,
`Catalog` lists `RaiseTargets`, `MacroEngine` sends "a Raise frame" and calls
`Rav4Emitter::sendNow`, and `HuMediaSource` / `HuDialect` / `CanboxUartSink`
parse Raise inbound.

After slice 0 the rest of the firmware sees one interface, `Hu::Protocol`:

- **out**: the message table (id, name, period, on/off), the field list
  (targets), `tick(state, now)`, `sendNow(msg)`, its own tuning settings
  (registered under the protocol's id, today `raise.*`);
- **in**: bytes → the head unit's values as signals (media text, volume,
  source, keys, clock), the dialect verdict, the ack;
- **keys out**: the key frame for a key code (today in `CanboxUartSink`).

`raise-rav4` implements it; the protocol is picked once at boot from
`hu.protocol`. No behaviour change: host tests and the sim stay green, and the
bench board sends the same bytes before and after.

## Slice order (replaces the list above)

0. HU protocol module (above).
1. Names and source classes; config file `/cfg/car.json` in LittleFS,
   loaded into PSRAM; `decode` / `huFmap` / `alerts` migrate out of NVS.
2. Rule engine: condition tree, FOR, ELSE, actions popup + set; alerts migrate
   into rules, `AlertRules` / `AlertEval` / `alerts` are deleted.
3. User signals.
4. Reminders as factory rules; `Derived` and Settings › Reminders lose them.
5. HU and phone signals (inbound through `Hu::Protocol` and the BLE services).
6. The editor «Мапа › Правила».
7. Head unit: «одразу при зміні» + gap, `hu <message> now`, user signals in
   «Поля ГУ».
8. Factory profile as JSON in the image, export / import, docs.
