# Car test: the X84 broadcast signals

What the 2026-10-20 build needs from one session in the car. Every row below
is a `DF_HYPOTHESIS` row in `src/vehicle/decode_defaults.inc`, positioned from
Renault's X84 broadcast matrix and never measured on our Mégane II.

**Before you start**
- If the map was ever saved on Авто → RAV4 → ГУ, NVS hides the new factory
  rows: press "Заводські прив'язки" once.
- Turn the recorder on (Авто → Запис). Put a marker on each step: the capture
  is what settles a wrong bit afterwards.
- Open Авто → Наживо on the phone. It shows every signal with its age and a
  ✓ or ? mark.

| # | Do | Watch on Наживо | Row |
|---|---|---|---|
| 1 | Key off: handbrake on/off ×3 | Ручник | `0x5C5` d0 bit2 |
| 2 | Lock/unlock with the remote | Замки (0-3) | `0x60D` d2 bits4-3 |
| 3 | Ignition on: brake ×3, clutch ×3 | Педаль гальма, Зчеплення | `0x181` d5 bit0 / bit3 |
| 4 | Accelerator released, then full | Педаль газу 0 → ~100 % | `0x181` d3 |
| 5 | Heated rear window, A/C | Обігрів заднього скла, Компресор (UPC), Клімат: A/C | `0x625`, `0x374` |
| 6 | Belts on/off, passenger sits down | Ремінь водія/пасажира, Пасажир | `0x651` d1 |
| 7 | Roof (CC) open/close | Дах: положення | `0x684` d0 |
| 8 | Engine running | Напруга АКБ ≈ 13.5-14.5 V, Лампа заряду off | `0x625` d2, d3 bit5 |
| 9 | Neutral, then each gear at a steady speed | Нейтраль, then Співвідношення (write each gear's value down: it calibrates Передача) | `0x215`, Derived |
| 10 | Reverse | Задня передача (UPC) and the old one | `0x215` d1 bit6 / `0x60D` |
| 11 | Compare with the cluster | Пальне, л vs the gauge; Одометр щитка; До ТО | `0x5C5` d7, d1-3; `0x715` |
| 12 | When the low-fuel lamp lights | a "Car alert / fuel_low: 1 ?" popup on the display | `0x5C5` d0 bit5 + alert rule 1 |
| 13 | Before/after refuelling | Пальне, л in both copies | `0x5C5` / `0x715` |
| 14 | A drive of 5 km+ | Витрата зараз/середня, Запас ходу, vs the cluster's trip computer | `0x551` d1 counter |

A row that reads right goes measured by dropping its `DF_HYPOTHESIS` (it then
shows ✓). A row that does not: the capture, the marker and the step number go
to the next session.

Head-unit experiments (Авто → RAV4 → ГУ → Експерименти ГУ, all off): switch
one on at a time and note what changes on the tablet's dashboard. The three
are coolant `0x7D 09`, fuel litres in `0x7D 04` D4-D5, and the computed range
and consumption in `0x21`/`0x22`/`0x23` (hu-decoder.md §2b).
