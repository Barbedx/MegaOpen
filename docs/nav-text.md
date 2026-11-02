# The navigation text screen: more text on glass, the clock kept

**The ask (2026-11-02).** A photo of a 2008 Mégane II with the Carminat
Guidage Informé shows the OEM panel in guidance: the left area carries TWO
rows of text in the small font (`>LITOMSKA, ULI` on the first row, 14 cells,
the rest of the street name wrapped to `CA` on the second), the clock and the
outside-temperature boxes stay under them, and the right part holds a vertical
bar (the distance to the turn) and the turn arrow. MegaOpen already draws the
right part (the 48×48 pane: the scenes) and the text in the big font, 8 cells.
The owner wants that layout: a long title on glass with the clock kept.

No capture of that screen exists. This page holds what is known, what was
tried, and the order to try the rest in, so a session in the car can be short.

## What the library builds (AffaDisplay v1.4.1)

| screen | id | bytes | on glass |
| --- | --- | --- | --- |
| text (`setText`, `styled`) | `0x151` | `10 0E 77 <icon> <bank2> <src> <fmt> 01` + 14 cells | the declared length `0x0E` = 14 content bytes: the panel consumes the header and the FIRST EIGHT cells, so 7-8 characters show in the big font (`CarminatDisplay.cpp`, "do not fix the declared length") |
| nav pane (the scenes) | `0x1F1` | `21 0B 00 00 00 00 00 00 00 00 00 12 30 30` + 48×48 bitmap (288 B) = 302 B | the right part of the photo; an independent layer, the info rows draw with it |
| menu | `0x151` | `21 01 ...` | a 26-column header in the small font, two 20-column rows in the big one |
| info rows | `0x151` | `10 0B 76 60 <offset> ...` | three rows of 8 cells, small font, a box |
| fullscreen | `0x151` | `21 05 FF 01 00 49 ...`, rows split by `0x0D` | the whole glass, 22 columns |
| popup | `0x151` | `10 .. 74 ...` | "VOL 28" overlay |

The format byte of the text frame is documented as a range (`0x19-0x3F` radio
style, `0x59-0x7F` plain ASCII; `0x60` is what we send) and the icon byte's
bit 7 is "unnamed in the origin and clear in every observed value": both are
unswept.

## What the OEM captures hold

`C:\Users\Andru\Downloads\Telegram Desktop\navi start.csv` and `nav still.csv`
(a Carminat navigation unit on the panel bus, August 2026; the same captures
the library's constants cite). `python tools/affa_capture.py "<csv>" [151,1F1]`
reassembles the ISO-TP messages and prints them with their ASCII.

- `navi start`: `0x151` `21 05 FF 01 00 49 ...` = the fullscreen "The route is
  / being calculated"; `0x1F1` `21 0B ... 12 30 30` + 288 B twice, an empty
  bitmap and then a diagonal arrow; `0x151` `25 00 03 00` / `25 00 00 00` every
  0.8 s (a blink or a heartbeat). The capture ends before the route exists.
- `nav still`: the sync (`3AF`/`3CF`) and the `25 00 0x 00` pair alone.

So the guidance text frame was never captured: the hypotheses below are the
way to it.

## Hypotheses, cheapest first

All of them run from **Сервіс › консоль дисплея** on the car's panel; no
firmware change is needed. `raw` takes a function id (`151` or `1F1`, the two
the panel registers) and the bytes as typed; `styled` is the text frame with
its four header bytes exposed. Photograph every result and note the console's
answer (the panel's ACK or the refusal).

1. **The declared length.** The content the length counts is the six header
   bytes (`77 <icon> <bank2> <src> <fmt> 01`) plus the text cells: `0x0E` =
   6 + 8, which is why eight cells show. The same frame declaring all
   fourteen, `raw` on `151`:
   `10 14 77 55 55 FF 60 01 3E 4C 49 54 4F 4D 53 4B 41 2C 20 55 4C 49`
   (`>LITOMSKA, ULI`). Then 22 cells, `0x1C`:
   `10 1C 77 55 55 FF 60 01 3E 4C 49 54 4F 4D 53 4B 41 2C 20 55 4C 49 43 41 20 54 45 53 54 20`
   (`>LITOMSKA, ULICA TEST `). If the panel shows more than eight cells,
   note the font and the rows.
2. **The format byte.** `styled`, 14 characters of text, `fmt` swept over
   `0x59..0x7F` and `0x19..0x3F`: a value may select the small font.
3. **The icon byte's bit 7.** `styled` with `icon` `D5` (`0x55 | 0x80`).
4. **The nav id.** The ordinary text frame on `1F1` (`raw`, id `1F1`,
   `10 0E 77 55 55 FF 60 01` + 14 cells): the panel may keep a second text
   area for the navigation unit, in the small font.
5. **The nav header.** The 302-byte pane message with bytes 2..10 of its
   header varied (the capture has zeros; the library's header carries `0x25`
   at byte 3): a flag there may switch the text area's mode.

What works goes into AffaDisplay first (a `showNavText()` with its constants
and a test), then MegaOpen takes it as a layout of the media screen: two small
rows for the title and the artist, the clock kept, the pane as now.
