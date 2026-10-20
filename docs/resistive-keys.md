# KEY1 / KEY2 — the resistive path to the head unit

Reference for `src/sinks/ResistiveKeySink.*`, `ResistiveLadder.*`, `ResistiveMap.*`
and `/api/res*` (no screen since ui-v2 slice 8). Everything below is graded: **CONFIRMED** means a primary
source or a hands-on measurement, **UNCONFIRMED** means it is the best available
guess and the firmware treats it as a setting rather than a constant.

The short version: the head unit's steering-wheel input is an ADC with a pull-up,
a button is a resistor to ground, and the unit *learns* whatever resistances you
present. So the design problem is not "what values does it expect" — you choose
those — it is staying inside the window it can resolve, and holding each value
still for long enough that its study page will accept it.

---

## 1. Wiring — CONFIRMED (DUDU7 pinout)

From the DUDU7 main power connector **[G]**:

| Pin | Label | Ours |
| --- | --- | --- |
| **G7** | Кнопки руля (вход 1) | **KEY1**, channel 0 |
| **G6** | Кнопки руля (вход 2) | **KEY2**, channel 1 |
| **G1** | GND, масса автомобиля | common ground — the ESP32 must share it |
| G4 | ILL, контроль подсветки | not used yet (see ARCHITECTURE §7.2) |
| G5 | Контроль заднего хода | not used yet |

There is no dedicated key-ground pin: the return is the vehicle ground on G1, so
the ESP32 and the head unit have to be on the same ground for any of this to
mean anything.

### Is KEY2 worth using? Sources disagree, and we design for KEY1

Three incompatible reports, none of them about a DUDU7 specifically:

* one bench test found both inputs behave **identically**, i.e. KEY2 buys nothing;
* an installer with a 10-button controller split across the two found **only one
  pod was ever recognised**, swapping the wires changed nothing, and the fix was
  to gang everything onto **KEY1 alone** — after which all ten worked;
* several installers describe KEY1 = left-hand buttons, KEY2 = right-hand.

The only claim with no counter-example is *KEY1 works*. So `BoardProfile.h` puts
**five legs on KEY1 and one on KEY2** — the second channel is wired as a
ten-minute experiment, not as half the design.

---

## 2. The electrical model

**CONFIRMED.** A resistive ladder to ground, read by an ADC with a pull-up to a
reference rail. Open circuit = full scale; a resistor to ground pulls the reading
down. Patent US8963364B2 describes exactly this, including a per-key value range
and a separate "no key" band. Head-unit SWC screens display the reading as
**0–255**, and idle sits at 254–255.

**MEASURED ON OUR UNIT, 2026-08-12 — and it settles the biggest open question:**

| Line | Idle | Consequence |
| --- | --- | --- |
| KEY1 / KEY2 | **2.8 V** | below the ESP32's 3.3 V rail, so a Hi-Z leg's ESD clamp never conducts — **bare GPIO is both safe and correct here, no transistors** |
| canbox UART | **3.3 V** | direct GPIO both directions; no divider, no series resistor on RX |

That is why `activeHigh` defaults to **false** (bare GPIO) and `series` to 30 Ω.

The sources had disagreed sharply and none of them was about a DUDU7: an
installer claiming 100+ units fitted insisted on **stabilised 5 V**; another put
the line at 3–5 V; the 3.3 V figure that circulates traces back to a blog about
somebody's own STM32 board. All of that is now moot for this car — but keep it
in mind for a different unit, because at 5 V the clamp *does* conduct and the
idle level loses about a quarter of full scale, which reads as a phantom press.
One multimeter probe on an idle KEY line settles it in either direction.

**UNCONFIRMED — the pull-up resistance.** No published value exists for this
class of unit. Worse, the patent's second embodiment uses a *switchable* pull-up
chosen to maximise the spread between neighbouring keys, and there are field
reports of the idle reading changing from 256 to 30 after changing
*Settings → Car Model → CanBox*. **Practical consequence: the input range is not
fixed, and a wrong car-model setting can put your whole ladder outside it.**

---

## 3. The window — the number that matters most

**CONFIRMED (two independent hands-on sources): the ceiling is about 5 kΩ.**
Above it, the input cannot tell a button from an open line.

This is the single most expensive thing to get wrong, because the failure is
*delayed*: a 10 kΩ rung learns perfectly on the study page and then never works
again, which looks like a firmware bug for as long as it takes somebody to
suspect the resistor. An early draft of `ResistiveMap.cpp` shipped exactly that
value. `ResistiveLadder::USABLE_MAX_OHMS` exists so the page says so.

> The 6.8k / 10k / 15k / 22k ladders on the forums are **Pioneer's**, off a 3.5 mm
> wired-remote jack. Different unit, different window. Do not port them.

**The floor is thermal drift, and it is why 20 Ω is the wrong answer.** One bench
source recommends ≥20 Ω between rungs. A driver whose buttons started *swapping
identity* after fifteen minutes of driving rebuilt his ladder around a **93 Ω**
delta. Another reports buttons firing anywhere across a 1.9–4.8 kΩ band as
supply voltage moves with ignition position. Seasonal re-learning is common
advice. **Use the 93 Ω figure**, and prefer the low end of the range where volts
per ohm is largest.

Real ladders published for this class, all ≤ ~4.3 kΩ: `100, 260, 470, 1.2k, 2.4k`
(Russian 5-button reference); `~150, 250, 500, 1000` (Ford Focus, measured);
`0 – 4.3k` span (Mazda 3, measured).

### What we ship

```
legs (KEY1): 150   390   820   1800   3300 Ω        + ~2 Ω series (MOSFET)
gaps:          240   430   980   1500
```

Five rungs, every gap far above 93 Ω, top rung well under 5 kΩ. They are handed
out **one leg per button**, because parallel combinations bunch: those same five
legs reach 31 values, and a dozen of them sit within a few percent of each other
down at the bottom. `ResistiveLadder::chooseMasks()` prefers single-leg rungs for
exactly this reason and only falls back to combinations when there are more
buttons than legs.

---

## 4. Drive: MOSFET or bare GPIO

Default is **MOSFET** (`activeHigh = true`), one small N-channel per leg —
2N7002, BSS138, anything — gate to the GPIO, drain to the KEY line through the
leg resistor, source to ground.

**The reason is not damage.** The obvious worry is that a 5 V line meets a
non-5-V-tolerant pin. That worry is misplaced: the head unit's own pull-up limits
the current, so the ESP32's clamp diode sees well under a milliamp whatever the
leg resistor is. Bare GPIO survives.

**The reason is correctness.** A leg sitting Hi-Z is not open — it is the KEY
line tied through the leg resistor to a clamp diode at ~3.6 V. On a 5 V unit with
a ~4.7 kΩ pull-up, that divider parks the *idle* line near 3.7 V instead of 5 V:
roughly a quarter of full scale gone, on an input whose entire job is to
distinguish "nothing pressed" from "some resistance". The unit can read a button
nobody pressed, and no amount of firmware will explain it.

Bare GPIO (`activeHigh = false`) is entirely correct on a unit **measured** at
3.3 V, and saves five transistors. It also raises the series term from ~2 Ω to
~30 Ω, which is 20% of the bottom rung — set `series` accordingly, the page has a
field for it.

---

## 5. Teaching the head unit — CONFIRMED, and not what you would guess

Primary source: the official DUDUAUTO wiki, page 方控学习 ("Square Control
Learning"):

> 选择要学习的功能，**长按方向盘按键3秒以上**，出现相应的图标既学习成功
> "Select the function you want to learn, **press and hold the steering wheel
> button for more than 3 seconds**, and the corresponding icon will appear."

1. Settings → vehicle protocol → **steering wheel key study**.
2. **Select the function on screen FIRST**, then hold the button. This is
   inverted from the usual assumption.
3. Hold **> 3 seconds**. This is a requirement, not a margin.
4. **Save once, at the very end.** Saving after each key is a documented trap —
   multiple users report only the last-programmed key surviving.
5. MODE is special on these units: it can cycle several apps, so pick exactly one
   if you want it to open one thing.

**What the routine demands of the signal — CONFIRMED across three sources.** It
must be *steady*. An SWC-interface manufacturer states it outright: "a steering
control interface needs to give out a **steady** resistance or voltage signal."
The patent stores a value only when a key is held "in a continued manner for a
prescribed time". No stepping toward the target, no bouncing, and a clean break
to open circuit at the end.

### The consequence that shapes the firmware

**A wheel detent lasts 60 ms and can never be held.** Without help, the encoder
is not awkward to teach — it is *impossible*, and the only symptom is a study
page that ignores you.

Hence `ResistiveKeySink::learnAssert()` and the 4-second teach hold,
`POST /api/res/learn`: it holds one rung flat for four seconds, bypassing routing and the queue,
un-preemptible by any key press. It is also how the ~26 spare rungs get taught,
which is what makes "extra emulated buttons" a thing you can do today rather than
a feature to build.

---

## 6. The encoder over a resistive line

Every detent has to become a press, and a press has a floor:

**CONFIRMED: 50 ms.** A shipping digipot adapter (`bigevtaylor/arduino-swc`)
pulses every button for exactly 50 ms. An independent ATtiny build arrived at 50
ms **empirically, because its head unit did not recognise anything shorter.**
Below 50 ms presses do not degrade — they vanish, with the same symptom as a
queue overflow. `pulseMs` is clamped at 50 in firmware, not just in the page.

**UNCONFIRMED: the inter-press gap.** No source names a required released-state
duration. The constraint is structural — the unit must *see* the idle level
between two presses — so symmetric with the press is the defensible choice.
Default 50 ms, giving a ceiling near **9 detents/second**. A flicked EC11 is
faster than that.

**CONFIRMED: holding works, and the head unit repeats.** The same adapter ships a
separate hold path, and factory menus expose long-press volume. AOSP's defaults
are **500 ms before the first repeat, then 50 ms** — though on these units the
SWC line is decoded by the MCU rather than by Android, so whether those numbers
apply here is **UNCONFIRMED**. They are settings.

### The three modes, and why the default is the hybrid

| `Spin` | Behaviour | Ceiling |
| --- | --- | --- |
| `Pulse` | one press per detent, fixed | ~9/s, exact, instant reversal |
| `Compress` | shrinks the **gap** as the queue deepens | ~13/s |
| `Hybrid` *(default)* | `Pulse` until the queue passes `holdAfter`, then hold | unbounded |

`Compress` cannot shrink the pulse — 50 ms is a hard floor — so it buys about
30%, which is why it is not the default.

The hybrid crossover is arithmetic, not taste. Pulsing N detents costs
`N × (pulse + gap)` = `N × 110 ms`. Holding costs `repeatStart + N × repeat` =
`500 + N × 50`. They are equal at **N ≈ 8**, so `holdAfter` defaults to 8 — below
that a hold is *slower*, because half a second passes before the unit repeats
anything, and a two-click nudge under a hold feels broken.

**The hold length is computed from the detents owed**, not fixed. A flat 1.5 s
hold at a 50 ms repeat delivers ~20 volume steps for the 8 the driver asked for.
`startNextDetent()` uses `repeatStartMs + queue × repeatMs`, capped.

---

## 7. Before building anything — two things to check first

1. **Does your DUDU7 have a native encoder mode?** A Hackaday commenter states
   that cheap Android head units "basically all have not one but two dedicated
   quadrature encoder inputs" with software support on KEY1/KEY2, configured in
   Factory Settings. **UNCONFIRMED — a forum comment, not documentation.** But if
   true, the whole press-emulation above is unnecessary for volume, and it costs
   ten minutes in a settings menu to find out. There is also a DUDU-specific
   forum thread, *"Rotary Dial / Volume Knob Remapping support"*
   (`forum.dudu-auto.com/d/2035`), which the research could not reach.

2. **Characterise the line.** Open the SWC study screen and sweep a decade box
   from KEY1 to ground, recording the displayed 0–255 value against resistance.
   One afternoon settles the pull-up rail, the real ceiling and the real
   resolution — every UNCONFIRMED row above — and the ladder's fields (`POST /api/res`)
   then take the measured values with no reflash.

Also worth knowing: `Steering wheel keys assign` must be `Can Key` rather than
`SWC Key` on some MTCD/MTCE units or the study app cannot change anything at all.
And long-press as a *distinct learned function* is vendor-dependent — Teyes and
Eonon have it, Joying does not, DUDU7 unknown. Our own `longMask` does not depend
on it: we present a genuinely different resistance, so the unit sees a different
button rather than a long press.

---

## 8. Sources

Primary: [DUDUAUTO wiki, 方控学习](https://wiki.dudu-auto.com/en/%E7%B3%BB%E7%BB%9F%E5%8A%9F%E8%83%BD/%E6%96%B9%E6%8E%A7%E5%AD%A6%E4%B9%A0) ·
[US8963364B2](https://patents.google.com/patent/US8963364B2/en) ·
[bigevtaylor/arduino-swc](https://github.com/bigevtaylor/arduino-swc) ·
[AOSP ViewConfiguration](http://www.dre.vanderbilt.edu/~schmidt/android/android-4.0/frameworks/base/core/java/android/view/ViewConfiguration.java)

Measurements and field reports: [The Chez](https://thechez.car.blog/tag/steering-wheel-controls/) (5 kΩ ceiling, 20 Ω step, KEY1≡KEY2) ·
[Incartec](https://incartec.blob.core.windows.net/instructions/Learning%20patch%20lead%20data%20sheet.pdf) ("steady signal") ·
[android-headunits.com](https://forum.android-headunits.com/viewtopic.php?t=45) (10 keys on KEY1 alone) ·
[drive2 — при прогреве слетают кнопки руля](https://www.drive2.ru/l/618702890604720837/) (thermal drift, 93 Ω) ·
[4PDA](https://4pda.to/forum/index.php?showtopic=1084167&st=600) (save once; adjustable voltage step) ·
[Yavilevich](https://blog.yavilevich.com/2020/06/repurposing-unused-steering-wheel-buttons-as-generic-macros-for-an-android-head-unit/) (0–4.3 kΩ) ·
[Hackaday](https://hackaday.com/2025/01/17/android-head-unit-gets-volume-knob-upgrade/) (native encoder claim, in the comments)

XDA and 4PDA block automated fetches, so the two richest threads on press timing
were read only in fragments. If a number above is marked UNCONFIRMED and you need
it firm, those are where to look next — in a browser.
