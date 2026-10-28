# MegaOpen user guide

For the T-2CAN car build, as of v1.1.0. The web app is in Ukrainian;
labels below are written as the app shows them. Flashing and wiring:
[README](../README.md).

## First boot

1. Join the board's WiFi `MegaOpen-XXXX` (`XXXX` from the board's MAC
   address), password `megaopen`.
2. Open `http://192.168.4.1`. On **Плата → WiFi** add your home network.
3. At home the board is at `http://megaopen.local` (or the IP your router
   gives it). Android before 12 does not resolve `.local`: use the IP. A router's own
   name for the board (`megaopen.fritz.box`) is refused: use the IP or `.local`.
4. **Change the AP password on Плата now.** Until you do, anyone in WiFi range
   of the car can join with the public default (`megaopen`) and flash the
   board. It is also the OTA password; there is no other lock
   ([SECURITY.md](../SECURITY.md)).

The tabs: **Огляд** (status of every part), **Екран** (the car's display),
**Кнопки** (keys and knobs), **Авто** (car CAN, head unit), **Плата** (WiFi,
Bluetooth, updates, all settings). **Сервіс** appears with **Плата →
Інтерфейс → Режим розробника**.

**EN / UA** in the header switches the page between Ukrainian and English, for
this browser only; `?lang=en` in the address does the same. The English page
is new and screen by screen: until every screen is done, some text stays
Ukrainian.

## The panel menu

Settings and car data on the car's own display.

| do | how |
| --- | --- |
| open | hold the right knob (~0.6 s) |
| move | turn the right knob, or the stalk's track up/down |
| choose / toggle | press the right knob (or the stalk's OK) |
| leave a group | `< Back` |
| close | `< Exit` (the top row), hold the right knob again, or wait 45 s |

The root rows are **Display**, **Lights**, **Service**, **Car data**, the
board's address (`IP 192.168.x.y`, or `AP MegaOpen-XXXX` on its own hotspot)
and **Power off**. Pressing the address shows it full size with the
`megaopen.local` name, the same as when WiFi connects. A yes/no setting toggles on a press; a number or a list opens
in `[brackets]`: turn to change, press to keep. Changes are saved at once and
are the same values as on the web.

**Power off** switches the display off. Press either knob to switch it back
on; turning a knob still changes the volume. Volume keys work while the menu
is open; track keys move the menu instead.

Do not use the menu while driving.

## The map page

**Авто → Мапа → Відкрити.** The whole chain «CAN row → signal → head-unit
field» on one page, in three tabs: **Сигнали** (where a signal comes from),
**Поля ГУ** (what the head unit gets) and **Сповіщення** (alerts on the
display). The tab is in the address: `#map`, `#map:fields`, `#map:alerts`.

**Сигнали** lists every signal the board reads from the car's CAN
(listen-only): value, age, where it comes from, and **✓** (checked in a car)
or **?** (positioned from Renault's data, not yet checked). It refreshes
every 2 s. Under a signal's name, **→ поле ГУ** names the head-unit fields
that take it; press one to land on that field in **Поля ГУ**.

When you have checked a **?** signal in the car (the steps are in
`docs/car-test-can-signals.md`), press **✓ підтвердити** on its row. The `?`
then goes everywhere: the menu, the car screen, alerts and the G-метр.
**↺ скасувати** takes it back. «Заводські прив'язки» turns every confirmed
signal back into a **?**.

**Where a signal comes from** is changed here and only here: press the source
chip on its row. A CAN row (ID, byte, length, shift, mask, × ÷ +), a constant,
or nothing. **Запам'ятати** then **Показати зміни** finds the byte: remember
the bus, do the thing (open a door), and the IDs that changed are listed.

**What the head unit gets** is the tab **Поля ГУ**: each field's
**«Звідки»** is its own signal (the default), another signal scaled
(× ÷ +, e.g. the battery `batt_v` ÷10 into the range field), a constant, or
off. Coolant, the P/R/N/D probe and «Бак, %» start off; switch one on with
«Звідки» → свій сигнал. «змінити в таблиці сигналів» (and the name of the
other signal) goes back to **Сигнали** with that signal's source editor open.
The tab also holds the protocol, «Слати в ГУ», the units, ▶ (a test value)
and «Прогін селектора».

If the list looks short after an update, see
[«Заводські прив'язки»](#after-an-update).

### Alerts on the display

The **🔔** beside a signal adds a rule and opens the tab **Сповіщення**
with the new row. Up to 16 rules:

| condition | fires when |
| --- | --- |
| **вище** / **нижче** | the value crosses the threshold; it fires again only after coming back past it |
| **зʼявилось** | the value goes up (for a yes/no: switches on) |
| **зникло** | the value goes down |
| **змінилось** | any change |

**Пауза** is the least time between two popups of one rule: at least 5 s
(a smaller number is saved as 5). Press **Зберегти правила**.

The one rule out of the box: the cluster's low-fuel lamp coming on, with a
5-minute pause. A popup never covers the open menu; it waits, and is dropped
if the condition is over by then.

## The car screen

Three car values on the display instead of now playing. **Екран → Екран авто**,
or in the panel menu under **Display › Car screen**:

* **Показувати замість музики** turns it on.
* **Що показувати:** **Свої рядки** (the three signals you pick in **Рядок 1-3**)
  or **Поїздка**: consumption now / average (L/100km), range, and the trip
  distance since the board started.

`?` after a value: not yet checked in a car; `--`: no fresh value (standing
still, consumption now is `--`).

## Media on the display

**Екран → Щиток.**

* **Розкладка:** **Текст** (8 characters, one row), **Інфо** (3 rows and the
  picture), **Меню** and **Повний екран** (3 rows of 22 characters). The
  preview shows what the glass will show.
* **Стиль** (every layout but Текст): класичний, мінімал, смуга, велика назва,
  тікер, виконавець зверху, пульс, сканер, великий годинник, рівень, потік,
  просто текст, вінамп, касета, друкарка.
* **Джерело тексту:** the head unit, the iPhone, or **авто** (the head unit
  while it sends text, otherwise the iPhone).
* **Картинка 48×48** (beside Текст and Інфо): one scene, or **карусель**
  through them all. Scenes: медіа, VU, хвиля, годинник, зорі, м'ячик, кола,
  спіраль, осцилограф, дощ, радар, життя, касета, тунель, G-метр, своя (draw
  it under **Намалювати свою**). G-метр and своя are not in the carousel.
* **G-метр:** the force you feel as a dot (a left turn pushes it right,
  braking up, speeding up down), a short trail, a ring at 0.5 g and the
  strongest pull held for 3 s. The ring is dotted while the acceleration
  signals are not yet checked in a car (like `?` on a text row); with no fresh
  data it is a dotted ring and nothing moves.

## Keys

**Кнопки → Куди йдуть натискання**: a grid of key groups against where they
go: **ГУ** (the head unit over UART), **iPhone** (media control over
Bluetooth), **HID** (the board as a Bluetooth keyboard), and the others your
build has. Each column's chip says whether that path is live now. A sink only
takes the groups it has commands for. The knobs' turns, presses and holds are
bound on the same page (**Ручка ліва**, **Ручка права**).

Defaults: the left knob is volume (press = mute), the right knob is the
menu's knob (hold = menu).

When the head unit is silent (switched off, or its wire to the board is
loose), volume keys go to the iPhone instead, after the board's first minute
of uptime, and back to the head unit as soon as it talks again. The grid then
shows **ГУ** as not live, with the reason under it.

The knob rings take WS2812B (GRB), RGB or SK6812 **RGBW** strips: **Кнопки →
Підсвітка → Тонко → Порядок кольорів**. On RGBW the white LED carries what
the three colours have in common.

## Updating

**From GitHub** (the board on a network with internet):
**Плата → Оновлення прошивки → З GitHub**.

1. **Перевірити** shows the latest release and whether it is newer.
2. **Оновити** downloads it and checks its SHA-256.
3. When it is written, **Перезавантажити**. This is refused while the car is
   moving; when the car stops, the board reboots into it by itself.

**From a file:** download `megaopen-t2can-<ver>.bin` from
[Releases](https://github.com/Barbedx/MegaOpen/releases) (`-rcN`
pre-releases have not run on a board yet), then on the same card choose the
file and **Оновити**.

Either way the new image runs on trial: kept after 60 s of running healthy;
if it crashes or is not healthy within 3 minutes, the board goes back to the
old one.

### After an update

If a signal's binding was ever saved, the board keeps that saved list of
bindings, and signals added by the update do not show. Press
**Заводські прив'язки** once, above the signals table (**Мапа › Сигнали**). It
replaces your own bindings with the new factory list; the head-unit map
(«Звідки») is not touched.

## The app on the head unit (MegaOpen Companion)

Install **MegaOpen Companion** (`megaopen-companion-X.Y.Z.apk` from the
release, Android 7 or newer, no root) on the head unit. It says the panel's
alerts out loud («Lights off!», «Handbrake on!», low fuel, your own rules),
shows the board's settings page, and is the board's way to the internet.

- **USB:** plug the board into the head unit's USB. Android offers to open the
  app; tick «Always» and it never asks again. Nothing to type.
- **WiFi:** the board on the head unit's hotspot. The app finds it (Ще › Зв'язок
  › address empty); type the IP from the panel menu if it does not.
- **Головна › Перевірка:** the board raises a test alert and the app says it:
  the board, the connection and the voice in one go.
- **Голос:** language (Ukrainian, English, the system's), volume, each alert on
  or off with your own phrase (`{v}` is the value). No Ukrainian voice: install
  it in «Налаштування синтезу мовлення» (the Google engine has one).
- **Repeat and notification:** each alert can be said once or again every 15,
  30 or 60 s while it lasts (lights and handbrake: 30 s); a notification stays
  until it is fixed. The speeds the reminders start at: the board's settings ›
  Нагадування (10 and 5 km/h; 0 switches one off).
- **Плата:** the board's own page, over USB or WiFi alike.
- **Ще › Оновлення:** from the same releases as the board; Android asks to
  confirm the install, and the first time to allow installs from this app.
- The head unit closes it in the background: exempt it from battery
  optimisation or put it on its auto-start list.

## Settings to a file

**Плата → Налаштування у файл.** **Зберегти у файл** downloads every setting
and the knob configuration as a `.json` (no passwords). Choosing a file
checks it first and shows what would change, what needs a reboot, and what
this board does not have (skipped); a bad value lists every problem and
changes nothing. **Застосувати** applies it all at once. A file never sets the
AP password. A file from a newer firmware is refused; keys this firmware does
not know are skipped.

## When something is wrong

**Огляд** shows each part as ok / idle / off / broken. The same comes as
JSON from `http://<board>/api/health`: one entry per part with a `code`; the
words beside it are the page's (`w.<code>` in `web/i18n/uk.json`, English in
`en.json`, shown with **EN** in the header).

| code | Огляд says | means | try |
| --- | --- | --- | --- |
| `panel_unregistered` | реєстрацію втрачено: панель не завершує рукостискання | the display is not finishing its handshake | check CAN B wiring and power; reboot the board |
| `panel_foreign_master` | на шині синхронізація іншої магнітоли, а «Реєстрація функцій» увімкнена — MegaOpen мовчить (лише слухає), щоб не битися з нею; вимкніть реєстрацію, щоб малювати поверх магнітоли | another radio drives the display bus and function registration is on; MegaOpen stays silent rather than fight it | turn «Реєстрація функцій» off to draw over the radio; with no OEM radio fitted, report it (a false detection) |
| `panel_beside_radio` | магнітола веде шину, MegaOpen малює поверх неї (реєстрація функцій вимкнена) | the OEM radio owns the handshake, MegaOpen sends its text over it | nothing: this is the overlay setup |
| `panel_cluster_only` | на цій шині лише щиток приладів, панелі AFFA нема | only an instrument cluster answers on this bus, no AFFA display | CAN B is wired to the wrong bus |
| `media_off` | екран музики вимкнено | now playing switched off | **Екран → Показувати музику** |
| `media_no_panel` | панель не зареєстрована, нема де малювати | nothing can be drawn | see `panel_unregistered` |
| `media_no_phone`, `ancs_no_phone` | телефон не підключено / телефон не підключено | no iPhone connected | pair from the iPhone's Bluetooth settings |
| `clock_no_source` | нема ні телефона, ні часу від магнітоли | no iPhone, and no clock from the head unit for 2.5 min | pair the iPhone, or check the head unit's UART (health `hu`) |
| `media_no_track` | телефон підключено, трека ще нема | iPhone connected, nothing playing yet | start playback |
| `clock_waiting`, `ancs_waiting` | телефон підключено, часу ще не надіслав / підключено, сповіщень ще не було | connected, nothing received yet | wait; notifications need permission on the iPhone |
| `clock_off` | автоматичний час вимкнено | automatic time is off | **Екран → Час автоматично** |
| `hu_off` | UART до ГУ вимкнено | head-unit UART switched off | **Кнопки → ГУ по UART** |
| `hu_silent` | від ГУ жодного кадру з запуску | no frame from the head unit since boot | check the UART wiring and the head unit's canbox profile (Toyota RAV4, 38400) |
| `hu_quiet` | ГУ замовкло | the head unit stopped talking | head unit asleep or a loose wire |
| `hu_dialect_vw` | ГУ говорить профілем VW, а плата — Toyota RAV4: у налаштуваннях авто на ГУ вибери Toyota RAV4 | the head unit is wired and talking, but in the VW canbox profile | in the head unit's car settings, choose the Toyota RAV4 canbox |
| `keys_none` | жодного натискання з запуску | no key since boot | press one; check the panel and knob wiring |
| `knobs_no_expander` | PCF8574 не знайдено: кабель QWIIC 2, живлення перехідника | the knob board is not found | QWIIC cable to the PCF8574 |
| `lights_off`, `lights_fail` | вимкнено: Кнопки → Підсвітка / драйвер стрічки не запустився | knob rings off / not starting | **Кнопки** light settings; ring wiring |
| `phone_ble_off`, `phone_advertising` | Bluetooth вимкнено / видно, але не підключено | Bluetooth off / waiting for a phone | **Плата → Bluetooth**; pair |

If the iPhone will not pair while it is on the board's own WiFi, leave that
WiFi and try again. For more detail: **Плата → Журнал запусків** (a log per
boot), and **Сервіс → Лог** (live).

## For developers

**Плата → Інтерфейс → Режим розробника** shows the **Сервіс** tab (the board
keeps the switch until its next reboot).

## Coming next

* **The board's IP on the display** when it joins a WiFi network, for phones
  that cannot open `megaopen.local`.
