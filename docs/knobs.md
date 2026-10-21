# The two front-panel knobs and their backlight

Reference for `src/knobs/*`, `src/input/QuadDecoder.*`, `src/input/PotStepper.*`
and the knobs on the Кнопки screen (`#keys`, `/api/knobs`). Build flag: `FEATURE_KNOBS` — on in `t2can`, off
in `direct`.

The decoding, the button, the pot and the light logic are host-tested
(`pio test -e native -f test_knobs`). On the T-2CAN the LED rings have been lit
(2026-10-18); treat the first power-up of anything else as commissioning, in the
order of §3.

---

## 1. What it is

| | part | wires | does |
| --- | --- | --- | --- |
| **right** | quadrature encoder with a push button (EC11 module: `KEY S1 S2 5V GND`) | 3 signals | CW / CCW / click / hold → any `Keys::Action`; defaults `EncCw` / `EncCcw` / `Ok` |
| **left** | a SECOND encoder of the same kind (the B10K pot lasted one evening: "резисторний хуйня, треба купити енкодер") | 3 signals | same; defaults `VolUp` / `VolDown` / `Mute` |
| **LEDs** | WS2812B chain: 0–8 behind each knob, then 0–144 in the footwells, one data pin | 1 signal | a colour **per LED** on the rings, one colour for the footwells, resting animations, a reaction to each knob, and a current limiter over the lot |

The bezel these go into (a printed part, not in this repository):
car-left is engraved `▽ VOL △`, car-right `PAUSE` / `◁ PREV` / `NEXT ▷`, and each
knob sits in a well with a clear printed light ring round it. The defaults below
are those engravings.

The knobs are a key **source**, beside the matrix and the OEM panel: their events
go through `KeyEngine::dispatch()`, so the macro filter sees them and the routing
matrix on the main page decides which sink (UART, KEY1/KEY2, AMS, HID) gets them.
Nothing about a knob is wired to one destination.

**Everything starts switched off** — encoder, pot and LEDs each have their own
enable on the Кнопки screen, stored in NVS. That is deliberate: the DevKit's inputs have no
pull-ups and a bare ADC pin wanders, so an image flashed onto a board with no
knobs attached would otherwise turn noise into volume keys at the head unit.
**Except on the T-2CAN:** its knobs sit behind the PCF8574, whose lines cannot
float (an open line reads high; no expander, no samples), so there both
encoders come up ON on an empty NVS. The LEDs stay OFF there too.

## 2. Wiring

Written pin → module label. The page shows the same table, read from
`BoardProfile.h` of the image that is actually running.

### T-2CAN

Both encoders go through a PCF8574 on QWIIC 2 (GPIO1 SDA, GPIO2 SCL): an LCD I2C
backpack, read one byte per millisecond by the knob task. Its P3 drives the
backpack's backlight transistor and always reads low, so it is skipped.

```
P0  <--  S1   (right)     LCD pin 4
P1  <--  S2   (right)     LCD pin 5
P2  <--  KEY  (right)     LCD pin 6
P6  <--  A    (left)      LCD pin 13
P4  <--  B    (left)      LCD pin 11
P5  <--  KEY  (left)      LCD pin 12
P7       spare            LCD pin 14
GPIO47 -->  DIN of LED #1   (U8 pad 19; 5 V from U8 pad 3)
```

The expander's address is probed at boot: 0x20-0x27 (PCF8574) and 0x38-0x3F
(PCF8574A). The LEDs stay on a GPIO: an I2C expander cannot time them. There is
no pot pin (`PIN_KNOB_POT = -1`); the pot code stays, host-tested, for a board
that wants one. `src/board/BoardProfile.h` is the source of truth; the drawing is
`hardware/schematic/t2can-qwiic.artifact.html`.

A footwell strip wants a 5 V supply of its own (100 LEDs: ~6 A white, ~2.5 A one
colour at full, 0.3–0.5 A at footwell-at-night levels), straight to the strip,
with its GND common with the board's. DIN over more than 30–50 cm wants a
74AHCT125 at the board and DIN run as a pair with GND.

### The three rules that are not optional

1. **The encoder module's "5V" pin goes to 3V3.** It is passive — contacts and
   pull-ups to that pin. Fed 5 V it pulls the ESP32's inputs to 5 V.
2. **The pot's ends go to 3V3 and GND**, never 5 V, and the wiper must be on
   ADC1. ADC2 does not work while WiFi is up.
3. **Only the LEDs get 5 V.** Put 100–1000 µF across their supply and a few
   hundred ohms in series with DIN. If the first LED misbehaves on a 3.3 V data
   line, that is the usual cause — a 74AHCT125, or a sacrificial first pixel fed
   through a diode, is the usual cure.

## 3. Commissioning order

The live readings (`/api/knobs/live`) work **before** that section is
enabled, so wiring can be proven with nothing bound to anything.

1. **Pot.** Watch the ADC number while turning. It should move smoothly end to
   end. Stuck at 4095 over the top quarter of the travel → the ADC only reads to
   ~2.5 V; add ~3.3 kΩ between 3V3 and the pot's top leg (for a 10 kΩ pot). Then
   "це мінімум" / "це максимум" at the two stops.
2. **Encoder.** Enable it. One click should add exactly 1 to "клацань". 2 or 4 per
   click → change "імпульсів на клац". "помилок" climbing with nobody touching
   the knob → a floating input (see the pull-up note above).
3. **LEDs.** Enable, set the counts. Red and green swapped → change the order.
   Colours apply live while dragged and are stored only by "Зберегти".
4. **Bindings last.** Defaults: encoder = `EncCw` / `EncCcw` / click `Ok`, pot =
   `VolUp` / `VolDown` in `steps` mode.

## 4. Behaviour worth knowing

* **Click vs hold.** With nothing bound to hold, the click fires on the way
  *down* (no latency). With a hold bound, the click has to wait for the release —
  until then it might be a hold.
* **Pot as volume.** Set the zone count to the head unit's number of volume steps.
  Turned fully down the knob then holds the unit at zero however far they had
  drifted, so the pair re-synchronises at every visit to an end stop. The first
  reading after boot is a position, not a movement: it sends nothing.
* **The lights are two layers.** The *resting* look: every LED has its own colour,
  and the zone is `static`, `breathe` or `rainbow` (rainbow ignores the colours).
  On top, the *reaction* to the zone's own knob, for `reactHoldMs` after the last
  movement:
  * `level` — the whole zone runs the gradient `lo → hi` with the knob;
  * `spin` — one LED is `hi`, the rest dim to ~25 %, and the lit one walks round
    with the knob: a click per LED for the encoder (it wraps), the position along
    the travel for the pot (it does not). Wants three LEDs or more; with one it
    just shows `hi` while the knob moves;
  * `flash` — every click kicks the resting look towards white.
  A button press flashes under every reaction.
* **How many LEDs per ring.** One LED under a 22 mm clear ring is a hot spot on
  one side. Three at 120° light it evenly, and are what makes `spin` worth having.
* **The encoder's gradient is virtual.** It has no position, so `level` runs over
  an imaginary travel `levelSteps` clicks long, clamped at both ends. It is a
  picture of the gesture, not a gauge of the head unit's real volume.
* **`reactHoldMs = 0`** makes the reaction permanent — the colour (or the lit LED)
  *is* the position. Otherwise the zone eases back to its resting look.
* **All eight colours of a zone are stored**, whatever its count says, so taking a
  ring from 3 to 1 and back on the page does not lose the other two.
* **The footwells are a third zone**, last on the same chain (the wire leaves the
  bezel and goes down to the floor): 0–144 LEDs, ONE colour, its own dimmer under
  the master, and the same three resting animations. It eases as one colour and
  one level rather than per LED, and the level starts at zero — the strip comes
  up from dark instead of snapping on.
* **The current limiter is the last thing done to every frame.** It adds up what
  the frame draws by the WS2812B's own arithmetic (20 mA per channel at full duty,
  1 mA per LED even when black) and, over `maxMilliamps`, scales every byte by one
  factor — dimmer, no hue shift. It covers the knob rings too: one budget for one
  supply. `/api/knobs/live` reports `mA` and `mAWanted`; the second above the
  first means it is dimming the chain right now.
  **The default is 400 mA — what a USB port gives.** Raise the cap only once the
  strip has a supply of its own.
* **`show()` does not wait.** A 160-LED frame is 4.8 ms on the wire and the caller
  is the task that samples the encoder every millisecond. The frame is handed to
  the RMT driver, which converts bytes to pulses a block at a time from its own
  interrupt (so a long chain costs 480 bytes, not 15 kB); a frame that finds the
  last one still going out is dropped and counted (`busy`, should stay 0).
* **Threads.** Pins, decoding, lights and the strip live on the 1 kHz `knobs`
  task; each key event is posted straight onto the KeyBus (a click as one item
  on the Tap lane, press and release together) and the `keys` task drains it
  and dispatches. `dropped` on `/api/knobs/live` counts posts the KeyBus
  refused — a full lane, so look at the keys task, not the panel task. The web
  task only swaps the config under a mutex; the knob task only try-takes it and
  keeps its last config while httpd holds it.
* **The PCF8574 (T-2CAN).** Read by the IDF I2C driver, not `Wire.requestFrom()`,
  whose `log_e` on every failed read spins ~6 ms on the console. Each transfer
  waits at most 2 ms. Five failed reads **in a row** drop the expander; the
  search then probes **one address per tick** (16 in all, ~0.1 ms each with
  nothing there), every second; a bus held low (a timeout) ends the sweep and
  backs off 1 → 8 s. Found again, the decoders and buttons are re-seeded, so a
  press held through the outage is neither a hold nor a click. Lost / found /
  stuck are logged on the change only, at most one line a second. A single
  missed sample mid-turn costs nothing: the decoder credits a two-contact jump
  to the direction the knob was going.
* **No allocation after boot.** The LED driver is 60 lines over the IDF's RMT
  channel 0 rather than a library, because the obvious library reinstalls the RMT
  driver inside every `show()`. A frame short enough to fit in RMT memory is sent
  with no interrupt involved at all.

## 5. API

| | |
| --- | --- |
| `GET /api/knobs` | the whole configuration, the pins, the action vocabulary; `enc` is the right knob, `enc2` the left (`fitted: false` on a board with no pins for it) |
| `POST /api/knobs` | any subset of it; `"save": false` applies without writing NVS |
| `GET /api/knobs/live` | detents, errors, button, pot raw/level/zone, LED frames, dropped events |

## 6. Not done

* SK6812 **RGBW**: Кнопки › Підсвітка › Тонко › Порядок кольорів = «RGBW (SK6812)».
  Four bytes a pixel, G R B W; the white LED carries min(r, g, b), taken off the
  three, and the strip gets SK6812 timing. The current limiter still counts
  three channels, so it over-estimates an RGBW strip (the safe side).
* The LEDs react to their own knob, not to volume keys from the stalk.
