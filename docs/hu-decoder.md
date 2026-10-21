# The head unit's own Raise decoder — what `com.syu.ms` actually parses

`raise-protocol.md` is built from vendor PDFs, third-party box code and bench
captures. This document is built from the **other end of the wire**: the
decompiled parser that runs on the DUDU7 and turns our UART bytes into UI state.
Where the two disagree, this one describes what the head unit *does*; the bench
still decides what it *shows* (the DUDU launcher that draws most of it is
encrypted, see §7).

**Source.** DUDUOS 3.8 beta `260929` for DUDU7 (official forum post
`d/4246`), OTA `payload.bin` → `odm.img` (EROFS) →
`odm/app/190000000_com.syu.ms/190000000_com.syu.ms.apk`, decompiled with jadx
1.5.1. ProGuard-obfuscated, **not** packed. Class names below are the
obfuscated ones in that build — they will change between builds; the logic will
not.

---

## 1. Where the bytes go

| Step | Class | What it does |
|---|---|---|
| UART | `chip.Chip`, `g.*` | canbox link is a fixed `/dev/ttySx` per SoC profile (`ttyS0/1/2/4`, `ttyHS1`). No USB path exists |
| Baud | `l0.b0` | protocol 112 → **38400**; 57600 only for car-ids 49-51, 56-58, 75, 111, 122, 133, 137-142 (none of the RAV4 ones) |
| Framer | `l0.v` | `0x2E`, then accumulates; checksum `~(cmd + len + Σpayload)` as a byte — matches §1 of `raise-protocol.md` exactly. Valid frame → `tt.f10921d.U2(buf, 1, len)` |
| Profile | `k0.wt.i0(id)` | `id & 0xFFFF` = protocol, `id >> 16` = car variant. **Protocol 112 → `k0.a3`** |
| Decoder | `k0.a3.U2` | one big `if`/`switch` on `cmd`; writes *update codes* via `wt.z0(code, value)` |
| UI | `com.syu.canbus` (`Callback_0112_RZC_XP1_Carmy`) and the DUDU launcher | receive update codes over IPC; also a second channel, update code **1049** `{index, value}`, which feeds the DUDU dashboard/status bar |

Every RAV4 entry in `FinalCanbus` is protocol `0x70` = 112, so they all land in
`k0.a3`:

| Constant | id | variant |
|---|---|---|
| `CAR_RZC_XP1_20RAV4` | `0x0C0070` | 12 |
| `CAR_DUDU_RZC_Toyota_RAV4_20_Hand` | `0x430070` | 67 |
| `CAR_DUDU_RZC_Toyota_RAV4_20_Auto` | `0x440070` | 68 |

`k0.a3` special-cases one id, `0x860070` (8781936), in several places
(`0x24`, `0x28`, `0x31` A/C, `0x58`). Do not pick that variant.

Inside `U2`, `bArr[i2]` is `cmd`, `bArr[i2+1]` is `len`, payload byte *n* is
`bArr[i2+2+n]`. Below, **D*n*** is payload byte *n*.

**There are no length guards.** No handler in `k0.a3` checks `len` before
reading (the one exception, `0x28`, uses it only to read *more*). A short frame
does not get dropped — the decoder reads past the payload into the checksum and
whatever is left in the buffer. Every "the HU drops frames whose length ≠ N"
statement in `raise-protocol.md` is about the RZC Java, not this unit.

---

## 2. Box → HU, as decoded

### 2a. Things that change what we send

| Cmd | Decoder reads | Consequence for us |
|---|---|---|
| `0x24` doors | D0: `b6` FL, `b7` FR, `b4` RL, `b5` RR, `b3` tailgate (`persist.syu.reversefrontdoor` swaps FL/FR). **D1: `b7` = bonnet (`U_DOOR_ENGINE`), `b3` = `U_DOOR_DRIVE_SIDE`.** D1 is read unconditionally. **No IG bit is read anywhere** | **With len 1, D1 is the checksum byte.** `2E 24 01 00 DA`: `0xDA` has b7 set → *bonnet open*. Send **len 2, D1 = 0x00**. The IG-gate theory (§4 experiment B) has no support in this decoder |
| `0x29` steering | 12-bit: `x & 0xFFF`, bit 11 = sign, magnitude `4096 − x`; full scale **±500** (`wt.k(..., false, 500)`). Sign-extended int16 decodes the same | Current encoding is right. **Ignored for the rest of the session once any `0x7D 08` has arrived** |
| `0x7D 08` steering | int16 LE, bit 15 sign; full scale **±540**; **opposite sign to `0x29`** (`wt.i(..., true, 540)`) | An alternative to `0x29`, not an addition. The HU *asks for it*: it sends `0x90 [7D 08 00]` (§3) |
| `0x28` | the **A/C** frame: D0 bits → A/C flags, D1 low nibble → fan (clamped 7), D2/D3 → L/R setpoint, D4 b0 = °F, **D5 = outside temp**, D6-D11 → more A/C state (D10-D11 only if `len ≥ 11`) | Our 12-byte "outside temp" frame works because D5 is where the decoder looks and everything else is zero = A/C off. Outside temp = `D5/2 − 40` °C (decoder: `D5×5 + 600` = tenths + 1000). D5 = `0x00` or `0xFF` clears it |
| `0x20` keys | `[code][state]`, table in §4; `state` 1 press / 0 release; codes 7 and 9 have their own branches (`7`: 1 press, 2 long, 0 release) | Not gated on anything: `tt.a()` is a hard `return true` |
| `0x1E` / `0x1D` radar | same `tt.a()` (always true). `0x1E` D4: `b7` on/off, `b6` distance, `b2-0` volume | The "send `0x1E` before `0x1D`" ordering rule is not in this decoder |

### 2b. The `0x7D` family — the full list

`raise-protocol.md` §0.6 says `0x7D` has no specification and the head unit is
the only authority. This is that authority. Sub-command is D0, data starts at D1.

| Sub | Layout (D1…) | Goes to | Status |
|---|---|---|---|
| `01` | lights mask: `b3` right ind, `b4` left ind (→ indicator UI), `b5` 136, `b6` 135, `b7` 137, `b1` 143, `b2` 144; whole byte → 1049 idx 11 | matches our mask; **`b1`/`b2` are read** — candidates for fogs | known, 2 unused bits now located |
| `03` | speed LE u16 ×100 → km/h (1049 idx 1); D3-D4 LE → code 97 | = ours | confirmed |
| `04` | odo LE u24 (1049 idx 20); **D4-D5 LE /10 → 1049 idx 5**; trip1 LE u24 (idx 21); trip2 LE u24 (idx 22) | the "`F2 08` constant, meaning unknown" is a **real field**: `0x08F2` = 2290 → 229. Likely range or fuel — find out by sending a different value | **new** |
| `05` | doors: `b7` FR, `b6` FL, `b5` RR, `b4` RL, `b3` tailgate, **`b2` bonnet** | a second door frame with the bonnet bit — no checksum trap | **new** |
| `06` | D1 `b0` → code 139 + 1049 idx 3, `b1` → 138, `b6` → 1049 idx 12; **D2 low nibble → 1049 idx 4** | layout of a status frame; a 4-bit field is the shape of PRND. Bench sweep needed | **new** |
| `08` | steering, see §2a | | **new** |
| `09` | **D2 − 40 → 1049 idx 2** (D1 unused) | a temperature in °C+40 on the dashboard channel — coolant is the obvious candidate (`0x551` b0 / `0x60D` b5 have it) | **new** |
| `0A` | D1-D2 LE u16 → code 154, **raw, unscaled** | the ×4 is applied by the consumer (launcher), not here — cannot be checked from `com.syu.ms` | = ours |
| `0D` | D1: `b2-3` → 145, `b4` → 146, `b5` → 147, `b6-7` → 148 | unknown | **new** |
| `F0` | D1…D6 raw → codes 255-260 | settings block | new |
| `F1` | D1 is tracked; value `0x11` triggers an app action, `0x01` after it another | not a data frame | new |

`0x7D 0B` (battery mV, from the Passat log) is **not** decoded in this build.

### 2c. The rest, briefly

| Cmd | As decoded | vs `raise-protocol.md` |
|---|---|---|
| `0x1F` | D0 `b7` → 98, low nibble → 164; D1 → 99; `(D0<<8)|D1` → 1049 idx 15 | agrees; still do not send |
| `0x21` | D0-D1 BE → avg speed, D2-D3 BE → elapsed, D4-D5 BE → range, D6 → unit (in the top byte of each) | agrees |
| `0x22` | D0 unit, D1-D2 BE; `0xFFFF` → 0 | agrees |
| `0x23` | D0 unit, D1-D2 current, then five BE16 at D3…D12 | agrees on len 13 |
| `0x25` | D0 status (`b7` present, `b6`, `b5`, `b2`, `b1-0` unit); D1-D5 → also 1049 idx 8 per wheel | agrees |
| `0x26` | four settings bytes | agrees |
| `0x27` | D0 unit, fifteen BE16 | agrees |
| `0x2A` | **second key table** (§4) | not in our doc |
| `0x2B` | D0 → 170, D1-D2 BE → 171, D3-D4 BE → 172 (`U_CARINF_EV_*`) | EV data, skip |
| `0x2F` | **third key table** (§4) | not in our doc |
| `0x30` | `new String(payload)` → version | agrees, any length |
| `0x31` | amp, nibbles as in our §2a | agrees |
| `0x32` | D0 `b2` → stored, `b3` → acts when `b2` set; `b0=1 && b6=0` → sends an MCU command | agrees on shape |
| `0x33` | **direct action**: D0 `1/2/3/4/5` → actions 34/35/32/33/29 (press+release in one frame) | **new** — a key path with no release frame needed |
| `0x41` | **decoded in this profile too**, into Toyota codes only: sub 1 D4-D7 body bits → 135-148 (the same codes as `7D 01`/`7D 0D`), sub 2 D1-D3/D6-D8/D9-D11 BE24 → 149/151/152, D4-D5 → 150, D12-D13 → 153 (speed), D14-D15 → 97; sub 3 D1-D2 BE16 → 154 + 1032, D7 → 155. **No battery, fuel or belt field** (the VW meaning does not apply) | `raise-protocol.md` §8 says "`0x41` is VW-only" — not on this unit |
| `0x4A`, `0x4B`, `0x58`, `0x61`, `0x62`, `0x66`, `0x1A`, `0xF2` | Crown/CD/radio/settings/PM2.5 | skip |

### 2d. Climate — `0x28` and `0x58`, bit by bit

Names are FYT's generic update codes from `FinalCanbus` (`U_AIR_*`), which the
protocol-112 A/C panel (`com.syu.ui.air.Air_0112_XP1_Camry`) draws from.

**`0x28` (len 12; D10-D11 read only if len ≥ 11)**

| Byte | Bits | Code | Meaning |
|---|---|---|---|
| D0 | b7 | 10 `AIR_POWER` | A/C system on |
| | b6 | 11 `AIR_AC` | compressor |
| | b5 | 12 `AIR_CYCLE` | recirculation (panel highlights "fresh" when 0) |
| | b4 | 13 `AIR_AUTO` | AUTO |
| | b3 | 26 `AIR_AUTO_RIGHT` | AUTO (2nd lamp, OR-ed with b4 on the panel) |
| | b2 | 14 `AIR_DUAL` | DUAL |
| | b1 | 15 `AIR_MAX_FRONT` | max front |
| D1 | b7 | 18 `AIR_BLOW_UP_LEFT` | vent: windscreen |
| | b6 | 19 `AIR_BLOW_BODY_LEFT` | vent: face |
| | b5 | 20 `AIR_BLOW_FOOT_LEFT` | vent: feet |
| | b3-0 | 21 `AIR_WIND_LEVEL_LEFT` | fan speed, clamped to 0-7. **`b4` (the spec's "display request") is not read** |
| D2 | | 27 `AIR_TEMP_LEFT` | setpoint, table below (`persist.fyt.reversetemp` swaps D2/D3) |
| D3 | | 28 `AIR_TEMP_RIGHT` | setpoint |
| D4 | b7 | 65 `AIR_FRONT_DEFROST` | front demist |
| | b6 | 16 `AIR_REAR_DEFROST` | heated rear window |
| | b5 | 54 `AIR_AQS` | air-quality recirc |
| | b4 | 52 `AIR_ECO` | ECO |
| | b3 | 53 `AIR_ACMAX` | A/C MAX |
| | b2 | 67 `AIR_REAR_LOCK` | rear panel lock |
| | b0 | 37 `AIR_TEMP_UNIT` | 0 °C / 1 °F |
| D5 | | 155 + status bar | **outside temp**, `D5/2 − 40` °C; `0x00`/`0xFF` = none |
| D6 | | 40 `AIR_REAR_TEMP_LEFT` | rear setpoint |
| D7 | b7 | 42 `AIR_REAR_POWER` | rear on |
| | b6 / b5 | 46 / 47 | rear vents face / feet |
| | b4 | 43 `AIR_REAR_AUTO` | rear AUTO |
| | b3-0 | 44 `AIR_REAR_WIN_LEV` | rear fan |
| D8 | | 41 `AIR_REAR_TEMP_RIGHT` | rear setpoint |
| D9 | | — | rear vent mode, only variants 63/76/90/91 |
| D10 | b1-0 | 166 `CAR_AIR_AUTOWIN` | AUTO fan level 1-3 (panel draws 3 widths) |
| | b4-2 | 86 | rear manual |
| | b7 / b5 | 76 / 92 | clean / front-only |
| D11 | b7 / b6 | 33 / 36 | auto flow / auto recirc |

Setpoint encoding with D4 b0 = 0 (°C) — value → °C:

| Byte | °C |
|---|---|
| `0x00` | LO |
| `0x1F` | HI |
| `0x01`-`0x1D` | `18.0 + (v−1) × 0.5` → 18.0 … 32.0 |
| `0x20`-`0x23` | 16.0, 16.5, 17.0, 17.5 |
| `0x24`-`0x25` | 15.0, 15.5 |
| anything else | "NO" |

With D4 b0 = 1 the byte is taken as whole degrees (°F). Rear setpoints use
`0x21`-`0x26` for 15.0-17.5 instead.

**`0x58` — seats and extras.** Not variant-gated in this decoder (only
`0x860070` skips it), so it is live on the RAV4 profile despite being "Crown
only" in the vendor spec:

| Byte | Bits | Code | Meaning |
|---|---|---|---|
| D0 | b7-4 / b3-0 | 29 / 30 | seat heat L / R (panel draws 0-3) |
| D1 | b7-5 | 87 `AIR_BLOW_MODE_RIGHT` | |
| D2 | b7-4 / b3-0 | 31 / 32 | seat ventilation L / R |
| D3 | b7 / b6 / b5 / b4 / b3 / b2 | 34 / 35 / 33 / 17 / 92 / 52 | nanoe / swing / auto flow / `AIR_FRONT_HOT` / front-only / ECO |
| D4 | four 2-bit fields | 88-91 | rear seat heat/vent |
| D5 | b2-0 | 66 `AIR_HOT_STEER` | heated steering wheel |

**Opening and closing the panel — there is no command for it.** Every A/C code
(10-96 and 166) is wired to `AirHelper.SHOW_AND_REFRESH`: **any change of any
value pops the A/C overlay, and it hides itself 5 s later.** Re-sending an
unchanged frame does not re-pop it. This is the OEM behaviour you want: send
`0x28` periodically from the car's state, and the panel appears exactly when
someone turns a knob. Two switches change it:

* the canbus app's "A/C window" setting (`sAirWindowEnable`) turns the overlay off;
* `persist.fyt.showduduair = true` hands climate display to the DUDU launcher
  (DUDUOS 3.8's "in-depth climate control integration"), which is encrypted —
  bench-test what it shows.

**What MegaOpen does about it (2026-10-21).** The outside temperature in
`0x28` (D5) is held: a new value is shown only after it has stayed for 60 s
(`src/proto/canbox/TempHold.h`), so a reading flickering between two degrees
no longer changes the frame. The A/C form (`raise.cmd28` = A/C) is sent only
when one of its signals is measured in the car or pinned by hand, never from
guessed rows. `/api/health` hu `climateChanges` counts the changed `0x28`
frames sent since boot.

One consequence for our current 12-byte outside-temp frame: once D0-D4 carry
real climate state, every fan/temperature change on the car will pop the
overlay. That is intended, but it is new behaviour.

### 2e. Is this every command?

Yes, for this profile. `k0.a3.U2` is the only consumer, and it handles exactly:
`0x1A 0x1D 0x1E 0x1F 0x20 0x21 0x22 0x23 0x24 0x25 0x26 0x27 0x28 0x29 0x2A
0x2B 0x2F 0x30 0x31 0x32 0x33 0x41 0x4A 0x4B 0x50 0x51 0x52 0x58 0x61 0x62
0x66 0x7D 0xF2`. Anything else is accepted by the framer and ignored.

`0xF2` is worth knowing about: sub `01` fires actions 72/71, subs `02`-`05`
are settings, and **sub `06` shows on-screen notices** (driver/passenger
seatbelt, individual doors open/closed …) — the texts are hard-coded Chinese,
so not useful as-is.

---

## 3. HU → box, as sent by `k0.a3`

All go out through `z.k.s(227, …)` — `227`/`0xE3` is the MCU routing prefix,
the rest is the Raise frame.

| Frame | When |
|---|---|
| `0x81 [state]` | ACC/start, then immediately: |
| `0x90 [30 00]`, `0x90 [32 00]`, `0x90 [25 00]` | requests version, system info, TPMS |
| `0x83 [32 01]`, `0x83 [39 W]` | two settings pushed at start |
| `0x90 [1D 00]`, `0x90 [1E 00]` | radar requests, only with the 360 camera app present |
| `0x90 [7D 08 00]` (**len 3**) | requests the `0x7D 08` steering frame |
| `0x84 [07 vol]` | volume, via a 37-entry lookup table `U0[]` — the value is a *mapped* level, not 0-63 raw |
| `0x99`, len 34 | **now-playing text** — see below |

`0x99` is built by `a3.c3(type, field, text)`: `[type][field][UTF-16LE, ≤15
chars, zero-padded]`, which is exactly `raise-protocol.md` §3b. Now with names:

| type | source | fields |
|---|---|---|
| `0x81`, `0x82` | radio | D1 band, D2-D3 frequency, then text `"xx.x Mhz"` (14 chars max) |
| `0x83` | media player | `01` → D2-D3 track no. LE, D4-D5 total LE (no text); **`02` title, `03` artist, `04` album** |
| `0x85` | Bluetooth (`"BT"`, `"BTAV"`) | same as `0x83` |
| `0x87` | AUX | `"AUX"` (header only in this build) |
| `0x88` | navigation | `"NAVI"` (header only) |

So field `04` album exists and we have not seen it yet.

---

## 4. Key codes → FYT actions

`0x20 [code][state]` looks `code` up in `a3.f3146k` and fires FYT action *n*.
Action ids are internal and unnamed; the right column is what the bench sweep
(`raise-protocol.md` §5) already tied to them.

| Code | Action | Bench | | Code | Action | Bench |
|---|---|---|---|---|---|---|
| `01` | 7 | vol+ | | `21` | 1 | — |
| `02` | 8 | vol− | | `22` | 12 | — |
| `03` | 4 | next | | `23` | 9 | — |
| `04` | 3 | prev | | `24` | 54 | — |
| `07` | own branch | source page | | `25` | 58 | — |
| `08` | 13 | voice | | `26` | 55 | — |
| `09` | own branch / 18 | answer | | `30` | 62 | — |
| `0A` | 26 | hang up | | `81` | 7 | vol+ (knob) |
| `0B` | 63 | — | | `82` | 8 | vol− (knob) |
| `13` | 3 | *bench: like `86`* | | `83` | 4 | next |
| `14` | 4 | *bench: like `85`* | | `84` | 3 | prev |
| `15` | 6 | back | | `85` | 34 | prev |
| `16` | 29 | enter | | `86` | 35 | next |
| `20` | 5 | — | | `87` | 16 | — |
| | | | | `88` | 2 | dashboard menu |

**One disagreement:** by the table `0x13` ≡ `0x04` (prev) and `0x14` ≡ `0x03`
(next); the bench note pairs them the other way. Re-check — it is two key
presses.

**Unexplored codes:** `0B 20 21 22 23 24 25 26 30 87` — ten actions nobody has
pressed, which is where mute and play/pause (§12 Q4) are most likely to be.
Two more tables widen the search: `0x2A [code][state]` (`f3158q`, adds actions
0, 21, 22, 38, 56, 71, 72) and `0x2F [code][state]` (`f3168v`, adds 53, 74,
75), plus `0x33 [1-5]` for one-shot 34/35/32/33/29.

---

## 5. Corrections to `raise-protocol.md`

1. `0x24` len 1 reads the checksum as D1 → spurious bonnet. **Use len 2.**
2. No IG bit is decoded; experiment B's premise is not in this decoder (the
   launcher could still gate on something — bench decides).
3. "The HU drops frames whose length ≠ N" — not this unit; nothing is length-checked.
4. `0x41` is decoded on the Toyota profile.
5. `0x7D 04`'s `F2 08` is a field (1049 idx 5), not a constant.
6. `0x29` full scale is ±500 here; `0x7D 08` is ±540 with the opposite sign, and
   its first arrival disables `0x29`.
7. `0x99` field `04` = album; field `01` = track number/total.
8. The `0x1E`-before-`0x1D` ordering rule is not in this decoder.

---

## 6. Bench list this produces, cheapest first

1. `0x24` len 2 with D1 = 0 — does a bonnet icon that was there go away?
2. `0x7D 04` with D4-D5 ≠ `F2 08` — which dashboard number moves? (Ready:
   the "пальне в 7D04" experiment.)
3. `0x7D 09 [00][coolant+40]` — does a temperature appear? (Ready: the
   "t° ОР" experiment.)
4. `0x7D 05` — same icons as `0x24`, plus bonnet?
5. `0x7D 06` D2 = 0…15 — PRND? Ready: switch on "селектор 7D06" (Авто →
   RAV4 → ГУ → Експерименти ГУ, `EXP_GEAR`), then "Прогін селектора" sends
   `0x7D 06 [00][n]` for n = 0…15, one value a step (3/5/8 s), through
   `/api/map/test` with id `prnd`. Note the D2 at which the tablet shows P, R,
   N or D; the emitter can then map `gear` onto it. Nothing builds a
   `0x7D 06` with the switch off.
6. `0x7D 01` with `b1`/`b2` — fog icons?
7. `0x20` with the ten unexplored codes, then `0x2A`/`0x2F`/`0x33` — find mute and play/pause.

---

## 7. What this does not cover

* **The DUDU launcher** (`com.dudu.autoui`) is packed with Tencent Legu
  (`libshella-4.6.2.2.so`, encrypted dex in `assets/0OO00l111l1l`). It is the
  consumer of update code 1049 and the dashboard, so the *meaning* of the 1049
  indices and the ×4 on RPM live there and are not readable statically.
* **`com.syu.canbus`** (UI side) is not packed and is decompiled, but it only
  sees update codes, never bytes.
* The `com.syu.ms` dex carries a 2022 timestamp. It is what ships in 260929; if
  the unit's canbus app is updated separately (that is what moves `0x7D`,
  §0.6), re-pull `190000000_com.syu.ms.apk` and diff `k0.a3`.
