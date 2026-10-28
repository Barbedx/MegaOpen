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
- Open Авто → Мапа → Відкрити on the phone (`#map`, tab Сигнали). It lists every
  signal with its value, age, source and a ✓ (measured) or ? (hypothesis).
- Optional, on the glass: Екран → "Екран авто замість аудіо" shows three
  chosen signals (rows 1-3) instead of now-playing, and the panel menu (hold
  the right knob) has Car data. Both mark an unmeasured value with `?`.

**How the yes/no rows read.** Brake, clutch, neutral and both reverse rows
are Renault 2-bit states: 01 is yes, 00 no, and 10/11 (reserved, not fitted,
invalid) leave the value where it was, so its age keeps growing on Сигнали.
Belts and the passenger seat are turned round by the firmware so that
**1 = buckled / somebody sitting**. `Замки` is raw: 1 boot locked, 2 doors
locked, 3 both.

| # | Do | Watch on Сигнали, expect | Row |
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
| 16 | A steady left turn, then hard braking in a straight line | `accel_lat` and `accel_long` on Сигнали: write down the sign of each. The G-метр (pick it on Екран → Картинка 48×48 → G-метр; it is not in the carousel) shows felt force, so its dot should go **right** in the left turn and **up** under braking. This confirms 0x2A0's signs | `0x2A0` b2.0/16, b1.0/8 |

**After a check passes, press «✓ підтвердити» on that row** of Сигнали (a
derived value has no button: confirm its inputs). The row is saved as measured
in this board's map, and the `?` goes everywhere: Сигнали, the panel menu, the
car screen, alerts and the G-метр's ring. «↺ скасувати» makes it a hypothesis
again. «Заводські прив'язки» brings back every hypothesis, so press ✓ again
after one. A row measured for everyone loses `DF_HYPOTHESIS` in
`decode_defaults.inc`. A row that does not read right: the capture, the marker
and the step number go to the next session.

**Alerts.** The bell 🔔 beside a signal on Сигнали adds a rule to the
Сповіщення tab of the same page (up to 16 rules: вище, нижче,
з'явилось, зникло, змінилось, with a pause of at least 5 s between popups). The card saves
through `/api/settings` (`alerts`) like every other setting; a bad cell is
refused with its row and column. The only factory rule is the low-fuel
lamp's rising edge. To try one without driving: add "Педаль гальма
з'явилось", then press the brake with the ignition on.

**Climate popup on the tablet.** Note `climateChanges` in `/api/health`
(chain `hu`) before and after a 10-minute drive, and count how often the
tablet's climate overlay pops up on its own. Each change of the `0x28`
frame may pop it; the outside temperature in it is now held for 60 s
against flicker, and the A/C form is sent only from measured or pinned
signals. Expect 0-2 changes and as many pops; more pops than changes means
something else opens it (write down what the car was doing).

**Head-unit fields that start off** (Авто → мапа ГУ, «Звідки» → свій
сигнал). Switch one on at a time and note what changes on the tablet's dashboard.
- **t° охолодної рідини:** coolant in `0x7D 09`.
- **Бак, %:** the tank in per cent x10 in `0x7D 04` D4-D5 (F2 08 while off).
- **range / consumption from the board:** «Запас ходу» ← `range_calc`,
  «Миттєва витрата» ← `cons_inst`, «Витрата за поїздку» ← `cons_avg`
  (`0x21`/`0x22`/`0x23`, hu-decoder.md §2b).
- **Селектор P R N D:** with it on, "Прогін селектора" steps `0x7D 06` D2
  through 0…15, 3, 5 or 8 s a value. Note the D2 at which the tablet shows P,
  R, N or D. This sends only to the tablet, never to the car.

- **Напруга АКБ у полі ГУ:** battery volts x10 in a field the tablet
  already shows; pick the field next to it (0x21 range, 0x7D 04, 0x23).
  Engine off, ~12.6 V: **which field shows 126 (or 12.6)?** Write down the
  field and the screen it is on; with the engine running it should read
  ~138-145 (13.8-14.5 V). It takes the field from «розрах. витрата» (0x21,
  0x23) or «пальне в 7D04» (0x7D 04), so those switch off while it is on.