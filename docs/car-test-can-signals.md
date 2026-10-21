# Car test: the X84 broadcast signals

What one session in the car needs to settle, for builds from 1.0.0 on. Every
row below is a `DF_HYPOTHESIS` row in `src/vehicle/decode_defaults.inc`,
positioned from Renault's X84 broadcast matrix and never measured on our
Mégane II. Doors, boot, lights and indicators are already measured.

**Before you start**
- If the map was ever saved on Авто → RAV4 → ГУ, NVS keeps the old rows and
  hides the new factory ones: press "Заводські прив'язки" once. Builds after
  1.0.0 also changed rows (steering centre, the 2-bit states, belts), and a
  saved map keeps those old too.
- Turn the recorder on (Авто → Запис). Put a marker on each step: the capture
  is what settles a wrong bit afterwards.
- Open Авто → Наживо → Відкрити on the phone (`#live`). It lists every
  signal with its value, age, source and a ✓ (measured) or ? (hypothesis).
- Optional, on the glass: Екран → "Екран авто замість аудіо" shows three
  chosen signals (rows 1-3) instead of now-playing, and the panel menu (hold
  the right knob) has Car data. Both mark an unmeasured value with `?`.

**How the yes/no rows read.** Brake, clutch, neutral and both reverse rows
are Renault 2-bit states: 01 is yes, 00 no, and 10/11 (reserved, not fitted,
invalid) leave the value where it was, so its age keeps growing on Наживо.
Belts and the passenger seat are turned round by the firmware so that
**1 = buckled / somebody sitting**. `Замки` is raw: 1 boot locked, 2 doors
locked, 3 both.

| # | Do | Watch on Наживо, expect | Row |
|---|---|---|---|
| 1 | Key off: handbrake on/off ×3 | Ручник 1 / 0 | `0x5C5` b1.5/1 |
| 2 | Lock/unlock with the remote | Замки 3 / 0 (1 or 2 if only one side locks) | `0x60D` b3.3/2 |
| 3 | Ignition on: brake ×3, clutch ×3 | Педаль гальма, Зчеплення 1 while pressed | `0x181` b6.6/2, b6.3/2 |
| 4 | Accelerator released, then full | Педаль газу ~0 → ~100 % | `0x181` b4.0/8 |
| 5 | Heated rear window, A/C | Обігрів заднього скла, Компресор (UPC), Клімат: A/C вибрано; Запит обігріву скла 2 or 3 when asked | `0x625`, `0x374`, `0x35D` |
| 6 | Belts on/off, passenger sits down | Ремінь водія/пасажира пристебнуто 1 / 0, Пасажир на сидінні 1 / 0 | `0x651` b2 |
| 7 | Roof (CC) open/close | Дах: положення 0 closed, 1 open, 2 closing, 3 opening | `0x684` b1.0/3 |
| 8 | Engine running | Напруга АКБ ≈ 13.5-14.5 V, Лампа заряду 0 | `0x625` b3.0/8, b4.2/1 |
| 9 | Wheels straight, then full left, full right | Кут керма ≈ 0.0, then several hundred degrees each way: write down which side is negative | `0x0C2` b1.0/16 |
| 10 | Neutral, then each gear at a steady speed | Нейтраль 1 in neutral; Співвідношення per gear (write each down: it calibrates Передача) | `0x215` b2.2/2, Derived |
| 11 | Reverse | Задня передача (UPC) and Задня передача 1, Передача R | `0x215` b2.0/2, `0x60D` b7.2/2 |
| 12 | Compare with the cluster | Пальне, л vs the gauge; Одометр щитка; До ТО, км; Рівень оливи (щиток) | `0x5C5` b8.1/7, b2.0/24; `0x715` |
| 13 | When the low-fuel lamp lights | a "Car alert / fuel_low: 1 ?" popup on the display (held while a menu is open) | `0x5C5` b1.2/1 + alert rule 1 |
| 14 | Before/after refuelling | Пальне, л in both copies | `0x5C5` / `0x715` b5.0/7 |
| 15 | A drive of 5 km+ | Витрата зараз/середня, Запас ходу (розрах.), vs the cluster's trip computer | `0x551` b2.0/8 counter |

A row that reads right goes measured by dropping its `DF_HYPOTHESIS` (it then
shows ✓). A row that does not: the capture, the marker and the step number go
to the next session.

**Alerts.** The bell 🔔 beside a signal on Наживо adds a rule to the
"Сповіщення на дисплей" card on the same page (up to 16 rules: вище, нижче,
з'явилось, зникло, змінилось, with a pause between popups). The card saves
through `/api/settings` (`alerts`) like every other setting; a bad cell is
refused with its row and column. The only factory rule is the low-fuel
lamp's rising edge. To try one without driving: add "Педаль гальма
з'явилось", then press the brake with the ignition on.

**Head-unit experiments** (Авто → RAV4 → ГУ → Експерименти ГУ, all off).
Switch one on at a time and note what changes on the tablet's dashboard.
- **t° ОР:** coolant in `0x7D 09`.
- **пальне в 7D04:** fuel litres in `0x7D 04` D4-D5.
- **розрах. витрата:** the computed range and consumption in
  `0x21`/`0x22`/`0x23` (hu-decoder.md §2b).
- **селектор 7D06:** with it on, "Прогін селектора" steps `0x7D 06` D2
  through 0…15, 3, 5 or 8 s a value. Note the D2 at which the tablet shows P,
  R, N or D. This sends only to the tablet, never to the car.
