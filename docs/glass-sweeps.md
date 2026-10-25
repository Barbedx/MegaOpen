# Glass sweeps

How wide the Carminat glass really is, measured with a phone camera. About
10 minutes, ignition on, parked. Results go into `docs/BACKLOG.md` ("Info list
truncates to 8 characters", "Column widths are guesses elsewhere").

## Setup

1. Phone on the board's WiFi (or the same network), the web app open.
2. **Плата → Інтерфейс → Режим розробника** on, then **Сервіс**.
3. The panel menu closed and no phone notification about to arrive (they
   would cover a step).

## Run

**Сервіс → Заміри скла → Почати.** Each **Далі** puts the next test on the
glass and holds it there. Photograph the whole glass, square on, then press
**Далі**. **Скинути** (or the last **Далі**) hands the glass back.

The rulers: a digit or letter every 10th position is its tens count, so
`123456789A123456789B12` is 22 long, and the last character you can read
gives the width directly.

| step | on the glass | write down |
| --- | --- | --- |
| 1 | fullscreen, row 1 `123456789A123456789B123456789C` | last digit/letter visible |
| 2 | fullscreen, row 1 `WWWWWWWWW1WWWWWWWWW2…` (the widest glyph) | last visible |
| 3 | fullscreen, row 1 `abcdefghi1abcdefghi2…` (lowercase) | last visible |
| 4 | fullscreen, three rows of 22 digits | all three end in `B12`? |
| 5 | menu: header digits, row 0 digits, row 1 W | last visible in each |
| 6 | info list: `12345678`, `WWWWWWWW`, `abcdefgh` | how much empty room is left in each row's box, in characters |

Each step is the same as a web request, for a laptop instead of the page:

```
B=http://192.168.4.1
curl -X POST -H "X-MegaOpen: 1" "$B/api/display/fullscreen?l1=123456789A123456789B123456789C"
curl -X POST -H "X-MegaOpen: 1" "$B/api/display/menu?header=123456789A123456789B123456789C&row0=...&row1=..."
curl -X POST -H "X-MegaOpen: 1" "$B/api/display/info?l1=12345678&l2=WWWWWWWW&l3=abcdefgh"
curl -X POST -H "X-MegaOpen: 1" "$B/api/display/clear"
```

## What the numbers set

* Steps 1-3: `affa::carminat::kFullscreenCols` (22 today) is the W width if
  W is narrower than the rest; the menu and fullscreen media layouts use it.
* Step 4: three rows of 22 fit the 73-byte body (expected: yes).
* Step 5: the menu layout's width and the header's.
* Step 6: the room in the box at the library's 8 cells
  (`10 0B 76 60 <offset> t0..t7`).
* Steps 7-15 (a longer declared info row, through a dev-only route) ran on
  2026-10-25 and showed 8 at every length; the route is gone since 1.5.2.

## Results, 2026-10-25 (T-2CAN in the car, owner's photos)

| step | seen |
| --- | --- |
| 1-3 | fullscreen row: **22** for digits (`…B12`), W (`…W2WW`) and lowercase (`…i2ab`) alike: no wide-glyph loss |
| 4 | three rows of 22, all end in `B12` |
| 5 | menu: header **26** (`…B123456`, small font), row 0 **20** (`…89B`), row 1 W **20** (`…W2`); the rows are the big font, cut by the scroll bar |
| 6 | info list: all three 8-character rows whole, the highlight box room for ~5 more |
| 7-15 | info row declared 9 to 16 cells, one row and all three: **8** every time, so 8 is the width |

Applied: `MenuUi::ROW_COLS = 20` (the panel menu's item rows) and
`MediaScreen::COLS_MENU = 20` (the media menu layout), 1.5.2. AffaDisplay's
`menuRowChars = 26` is the header's width; the rows are 20 (BACKLOG).
