# MegaOpen user guide

For the T-2CAN car build, as of v1.1.0. The web app is in Ukrainian;
labels below are written as the app shows them. Flashing and wiring:
[README](../README.md).

## First boot

1. Join the board's WiFi `MegaOpen-XXXX` (`XXXX` from the board's MAC
   address), password `megaopen`.
2. Open `http://192.168.4.1`. On **Плата → WiFi** add your home network.
3. At home the board is at `http://megaopen.local` (or the IP your router
   gives it). Android before 12 does not resolve `.local`: use the IP.
4. **Change the AP password on Плата now.** Until you do, anyone in WiFi range
   of the car can join with the public default (`megaopen`) and flash the
   board. It is also the OTA password; there is no other lock
   ([SECURITY.md](../SECURITY.md)).

The tabs: **Огляд** (status of every part), **Екран** (the car's display),
**Кнопки** (keys and knobs), **Авто** (car CAN, head unit), **Плата** (WiFi,
Bluetooth, updates, all settings). **Сервіс** appears with **Плата →
Інтерфейс → Режим розробника**.

## The panel menu

Settings and car data on the car's own display.

| do | how |
| --- | --- |
| open | hold the right knob (~0.6 s) |
| move | turn the right knob, or the stalk's track up/down |
| choose / toggle | press the right knob (or the stalk's OK) |
| leave a group | `< Back` |
| close | `< Exit` (the top row), hold the right knob again, or wait 45 s |

The root rows are **Display**, **Lights**, **Service**, **Car data** and
**Power off**. A yes/no setting toggles on a press; a number or a list opens
in `[brackets]`: turn to change, press to keep. Changes are saved at once and
are the same values as on the web.

**Power off** switches the display off. Press either knob to switch it back
on; turning a knob still changes the volume. Volume keys work while the menu
is open; track keys move the menu instead.

Do not use the menu while driving.

## The live car page

**Авто → Наживо → Відкрити.** Every signal the board reads from the car's CAN
(listen-only): value, age, where it comes from, and **✓** (checked in a car)
or **?** (positioned from Renault's data, not yet checked). It refreshes
every 2 s.

If the list looks short after an update, see
[«Заводські прив'язки»](#after-an-update).

### Alerts on the display

The **🔔** beside a signal adds a rule to **Сповіщення на дисплей** on the
same page. Up to 16 rules:

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

Three car values on the display instead of now playing: **Екран авто замість
аудіо**, and **Екран авто: рядок 1-3** for which signals. In the panel menu
under **Display**, or on **Плата → Усі налаштування**. `?` after a value: not
yet checked in a car; `--`: no fresh value.

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
  спіраль, осцилограф, дощ, радар, життя, касета, тунель, своя (draw it under
  **Намалювати свою**).

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

If the car page was ever saved (**Авто → RAV4 → ГУ**), the board keeps that
saved list of signal bindings, and signals added by the update do not show.
Press **Заводські прив'язки** once. It replaces your own bindings with the
new factory list.

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
JSON from `http://<board>/api/health`: one entry per part with a `code`.

| code | means | try |
| --- | --- | --- |
| `panel_unregistered` | the display is not finishing its handshake | check CAN B wiring and power; reboot the board |
| `media_off` | now playing switched off | **Екран → Показувати музику** |
| `media_no_panel` | nothing can be drawn | see `panel_unregistered` |
| `media_no_phone`, `clock_no_phone`, `ancs_no_phone` | no iPhone connected | pair from the iPhone's Bluetooth settings |
| `media_no_track` | iPhone connected, nothing playing yet | start playback |
| `clock_waiting`, `ancs_waiting` | connected, nothing received yet | wait; notifications need permission on the iPhone |
| `clock_off` | automatic time is off | **Екран → Час автоматично** |
| `hu_off` | head-unit UART switched off | **Кнопки → ГУ по UART** |
| `hu_silent` | no frame from the head unit since boot | check the UART wiring and the head unit's canbox profile (Toyota RAV4, 38400) |
| `hu_quiet` | the head unit stopped talking | head unit asleep or a loose wire |
| `keys_none` | no key since boot | press one; check the panel and knob wiring |
| `knobs_no_expander` | the knob board is not found | QWIIC cable to the PCF8574 |
| `lights_off`, `lights_fail` | knob rings off / not starting | **Кнопки** light settings; ring wiring |
| `phone_ble_off`, `phone_advertising` | Bluetooth off / waiting for a phone | **Плата → Bluetooth**; pair |

If the iPhone will not pair while it is on the board's own WiFi, leave that
WiFi and try again. For more detail: **Плата → Журнал запусків** (a log per
boot), and **Сервіс → Лог** (live).

## For developers

**Плата → Інтерфейс → Режим розробника** shows the **Сервіс** tab and tells
the board (until its next reboot) to accept the dev-only test routes:

* `POST /api/display/inforaw?len=0x0B..0x13&offset=0x41&prefix=0x60&text=…`:
  one info-list row with a longer declared length, for the glass sweep in
  [glass-sweeps.md](glass-sweeps.md). It goes once the sweep has its answer.

## Coming next

* **The board's IP on the display** when it joins a WiFi network, for phones
  that cannot open `megaopen.local`.
