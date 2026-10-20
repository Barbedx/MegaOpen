# Raise / RZC canbox protocol — implementation reference

**Target head unit:** DUDU7 (FYT, UIS7870), profile *Raise / RZC → Toyota Corolla-RAV4 2018-2020*.
**Target vehicle:** Renault Mégane II Ph1 CC (coupé-cabriolet), 2004, LHD.

Confidence legend used in every table:

| Mark | Meaning |
|---|---|
| **confirmed** | Observed working on *this* DUDU7 or *this* car (7 messages already shipped, or our own AFFA3 firmware) |
| **high** | Official Ruizhicheng spec, or two independent primary sources agreeing byte-for-byte |
| **medium** | One primary source only (spec-only, or one implementation only) |
| **low** | Constant name known, layout not recovered |
| **CONFLICT** | Primary sources disagree and the conflict is *not* resolved — both readings shown, with which to try first |

Nothing below is a measurement on our hardware unless it says **confirmed**. Everything else is document- or code-derived.

---

## 0. Source update — the two mikescotland PDFs, fetched directly

The "Unreachable / incomplete sources" section below used to record that mikescotland's Google Drive PDFs were never fetched and that vendored copies were used instead. **Both have now been read directly:**

* `RAV4_BODY_ENGINE_2018_2020.pdf` — 14 pages, twelve messages
* `SWC_RAV4_2018_2020.pdf` — 3 pages, framing + the full `0x20` key table

The SWC document reproduces §5's key table **verbatim, with no additions or corrections** — including `0x88` = MODE and the long-press-`0x07` = MUTE note. §5 needs no change.

The body/engine document produced four things worth acting on.

### 0.1 Nine of eleven worked examples verify; two do not

Recomputing `(cmd + len + Σpayload) ^ 0xFF` over each printed frame:

| Message | Document | Computed | |
|---|---|---|---|
| rpm, lights, doors, steering, outside temp, hybrid, TPMS, instant fuel, SWC vol+ | — | — | **all 9 agree** |
| speed `0x7D 03` | `0x34` | `0xDA` | **document wrong** |
| odometer `0x7D 04` | `0x5D` | `0xF1` | **document wrong** |

The speed example is wrong twice over: it states the sum as `0x1CB` when the bytes it lists sum to `0x125`, and it says "avg = 200.00 km/h" while writing `10 27` (= 10000, i.e. 100.00) into the average field — most likely the rpm example's bytes pasted in. **The framing rule is not in doubt**: nine independent examples confirm it, and this firmware reproduces all nine byte for byte (`test/test_vehicle`). Treat those two printed checksums as transcription errors, not as evidence of a second rule.

### 0.2 `0x29` steering — the encoding changed, and this is a real fix

The document's prose says "signed 16-bit in 0.1° units", but its own worked example writes

```
-26 deg  ->  2E 29 02 E6 FF EF
```

`0xFFE6` is −26, not −260, and the checksum `0xEF` verifies against exactly those bytes. Together with its stated ±540 range, the wire unit is **whole degrees**, and the upper byte is **sign-extended** — not masked to a nibble as the vendor spec's "D1 b7-4 ignored" 12-bit reading implies.

We were emitting the zero-extended 12-bit form. **Sign-extending satisfies both decoders**: a reader that masks to 12 bits sees `0xFE6`, whose bit 11 is the sign, and still reads −26; a reader that takes the int16 reads −26 directly. Zero-extending satisfies only the first, and turns every left-hand angle into a large positive one for the second — which is exactly the class of bug the "CONFLICT on sign" row below was worried about. Fixed; the worked example is now a golden vector.

### 0.3 `0x24` doors — len 1 vs len 2 is now a runtime switch

This document, the source of our only *confirmed* door icons, sends **len 1**. The vendor spec sends **len 2** and puts the ignition bit in D1 b1, and per that spec the IG bit gates the trip, fuel, history and TPMS panels from being drawn at all. The head unit guards on exact length, so this is not a degraded frame versus a good one — it is a frame versus a **dropped** frame, and the current code had silently switched to len 2, risking icons that already worked.

`POST /api/canbox/tuning {"doorsLen":1|2}` flips it (Сервіс screen, "Двері: довжина кадру"), which turns bench experiment B below into one setting.

### 0.4 `0x1F` hybrid — layout fully recovered

D0 b7 = is-hybrid (gates everything), D0 b6-0 = battery level (8 bars on the original). D1 = flow bitmask: b0 motor→batt, b1 motor→wheels, b2 engine→motor, b3 engine→wheels, b4 batt→motor, b5 regen (worked example `2E 1F 02 86 0D 4B`, verifies). §8's "do not emit" still stands — it is a policy about *our car*, not a gap — and it is enforced by the ordinary signal rule: nothing goes out until `hybrid_present` is pinned by hand.

### 0.5 `0x21` — the document reads it as remaining range only

It describes payload bytes 0-3 as padding always zero, 4-5 as remaining range (BE16) and 6 as a constant `0x02`. The vendor spec additionally reads 0-1 as average speed and 2-3 as elapsed minutes. **Same length, same range position, same trailing constant**, so our fuller form is a strict superset — worthless if the unit ignores the first four bytes, free information if it does not.

### 0.6 Source survey, 2026-08-12 — nothing has superseded §0

Every upstream this document is built from was re-checked before the first bench
session. **None has moved in a way that changes a byte here.** Recorded so the
next person does not repeat the search:

| Source | State on 2026-08-12 | Verdict |
|---|---|---|
| mikescotland's two PDFs | posted 2025-09-05 (dudu-auto `d/775` post 62), unchanged since | §0 is current |
| Vendor spec `睿志诚软件_丰田CAMRY & RAV4 串口通讯协议` | still **V2.21.000**. No V2.22+ exists anywhere findable, in English or Chinese | our version is the latest |
| dudu-auto thread `d/775` | 80 posts, last 2026-06-24 — but the last post with protocol *substance* is 2026-01-29 | captured below |
| `aerodomigue/esp32-canbox-nissan` | pushed 2026-06-28, but every 2026 commit is CI, OTA or steering-scale calibration. `docs/protocols/raise-toyota-rav4/` untouched since it was added, 2026-01-21 | no new tables |
| `smartgauges/canbox` | last push 2025-08-04 | unchanged |
| `RZC_ToyotaSeriesProtocol.java` (darkspr1te) | unchanged | unchanged |
| Mikescotland's GitHub account | **no canbox repository exists.** Post 64 (2025-09-08) promised "once finished, it will pop up on git"; eleven months later it has not | there is no fuller document to find |

**What "the protocol changes sometimes" actually means.** Post 77 (2026-01-29):
the UART protocol is *consistent* across FYT and other Android head units, and
what changes is that **new functions are added on address `0x7D`** — older head
units ignore the ones they do not know, newer DUDU UI draws the extra data.

Three consequences, and they are the reason this subsection exists rather than a
line saying "re-checked, fine":

1. **The drift is additive on one command byte, not a redefinition.** Nothing in
   §2a can silently become wrong under somebody else's firmware update. A `0x24`
   is a `0x24`.
2. **`0x7D` has no vendor specification at all** (see *Unreachable sources*), so
   the head unit on the desk is the only authority for it. That cuts both ways:
   there will never be errata, and an unknown sub-command is *safe to probe* —
   the failure mode is being ignored, not being misread. We ship `01` lights,
   `03` speed, `04` odo, `0A` rpm; `0B` is known from the XDA Passat log
   (battery millivolts) and is not implemented. `POST /api/canbox/raw` with
   `cmd=7D` reaches every one of them without a rebuild.
3. **`0x03` is still unsorted upstream.** Post 76 (2026-01-29) repeats that PRND
   and parking brake are scattered across ~128 bits of a single address and that
   the guide is incomplete there. §2a's "do not emit" stands unchanged.

**When to re-run this survey:** when the head unit's own canbus firmware is
updated (that is what moves `0x7D`), or if `d/775` gains a post from
mikescotland — not on a schedule. Nothing else here has an upstream that ships.

---

## 1. Framing and link parameters

### 1a. The UART is a 3.3 V interface — three independent sources agree

The vendor doc's "3.3 V or 5 V depending on the head unit's pull-up" reads like an open hazard. It is not, for our case:

* **`aerodomigue/esp32-canbox-nissan`** — the closest reference there is, since it targets **DuduOS / FYT**, the same head-unit family as ours — wires an **ESP32-C3 GPIO directly, both directions**: GPIO5 (TX) → radio-harness RX, GPIO6 (RX) ← radio-harness TX, 38400. No level shifter, no divider. Its only voltage warning in the whole document is about the CAN transceiver supply (*"Do not use 5V!"*), i.e. it treats the entire board as 3.3 V logic.
* **`st9r/esp32_arduino_canbox`** — a Raise/RISE implementation for **5 V** Arduinos — is the one that settles it by construction. It requires a divider *down* to the head unit: `Arduino TX --[1.8k]--+-- Head Unit RX` with 3.3k to ground (or 1k/2k). **A divider that turns 5 V into ~3.3 V exists precisely because the head unit's RX expects 3.3 V.** It also says outright: *"RX Arduino < TX Head Unit → Not Connected!"* — that project never connects the head unit's TX at all.
* **MeganeCAN's own gateway board** (`hardware/meganecan-gw`) carries the nets `GW_TX_HU_RX` / `GW_RX_HU_TX` and **no level-shifter part anywhere in its BOM** — passives, two SN65HVD230 and ESD diodes, nothing else.

**So: connect our TX (GPIO21) → head-unit RX directly. That is what all three do.**

The only residual risk is our **RX** (GPIO20), because "depending on the pull-up" is about the *rail the line idles at*, and the ESP32-C3 is not 5 V tolerant. Three mitigations, cheapest first:

1. **Measure it.** A UART idles HIGH, so a multimeter on the head unit's TX pin reads its logic-high rail directly. 3.3 → wire it; 5 → use 2 or 3.
2. **Run TX-only**, exactly as `st9r` does — leave the head unit's TX disconnected. Everything we are actually testing (`0x30` version, doors, keys, telemetry) is box→HU. What is lost is the `0x90` responder and the inbound volume on `0x84` idx `0x07`, both of which can wait.
3. **1 kΩ in series into GPIO20.** The pin's clamp diodes handle the rest, and 1 k is far below what 38400 baud cares about.



Every frame in both directions is `[0x2E][cmd][len][payload…][cs]` where `len` is the payload length in bytes and `cs = (uint8_t)(cmd + len + Σpayload) ^ 0xFF` — accumulate into a `uint8_t`, low byte only (verified against mikescotland's own worked example: sum `0x1CB` → `0xCB ^ 0xFF = 0x34`). Link is **38400 8N1, 3.3 V TTL**, direct ESP32-C3 GPIO to HU (canbox-nissan wires `HardwareSerial(1)` RX=GPIO6/TX=GPIO5; the official Chinese doc says "3.3 V or 5 V depending on the head unit's pull-up", and its debugging section states flatly that **pull-up resistors on TXD/RXD are required** — check those before suspecting firmware on a dead link). Frames are written back-to-back with no inter-frame delay and no TX queue; never byte-interleave two frames. Worked example, Vol+ key: `0x20+0x02+0x01+0x01 = 0x24`, `^0xFF = 0xDB` → `2E 20 02 01 01 DB`.

---

## 2. Box → HU messages

### 2a. Toyota / RAV4 profile — the profile we are targeting

Command space is `0x1D`–`0x32` plus `0x50`–`0x58` (official spec), **plus** the undocumented `0x7D` multi-function family that only exists in FYT/DUDU firmware.

| Cmd | Name | Payload layout | Interval | Profile | Confidence |
|---|---|---|---|---|---|
| `0x7D` `01` | Lights / indicators | len 2. `[0x01][mask]`; mask: `0x08` right ind, `0x10` left ind, `0x20` high beam, `0x40` low beam/headlights, `0x80` parking. Bits 0-2 unassigned, send 0. Ex: `2E 7D 02 01 D0 AF` | 200 ms + on change | rav4 | **confirmed** (ours) — layout **high** |
| `0x7D` `03` | Speed + average speed | len 5. `[0x03][speed LE u16][avg LE u16]`, both = km/h × 100. Ex 123.45 + 200.00 → `2E 7D 05 03 39 30 10 27 34` | 500 ms | rav4 | **confirmed** (ours) — the **trailing 2 bytes are average speed, not padding**, **high** |
| `0x7D` `04` | Odometer + trip1 + trip2 | len 12. `[0x04][odo LE u24, km][0xF2][0x08][trip1 LE u24, ×10 = 0.1 km][trip2 LE u24, ×10]`. Ex odo 123456, trip1 34.0, trip2 231.0 → `2E 7D 0C 04 40 E2 01 F2 08 54 01 00 06 09 00 5D` | 10 s | rav4 | **confirmed** (ours) — trip1/trip2 encoding **high**; `F2 08` constant, meaning unknown |
| `0x7D` `0A` | Engine RPM | len 3. `[0x0A][rpm×4 LE u16]`. Ex 2500 rpm → 10000 → `2E 7D 03 0A 10 27 3E` | 333 ms | rav4 | **confirmed** (ours) — **×4, not /2**, **high** |
| `0x24` | Base info: doors + ignition | **Spec: len 2.** D0 doors `b7` right-front, `b6` left-front, `b5` right-rear, `b4` left-rear, `b3` tailgate, `b2` power tailgate present, `b1` power-tailgate direction. **D1 key status, `b1` = IG (1 = ignition ON).** Ours + canbox-nissan send **len 1** (D0 only) | 250 ms + on change | rav4 | doors **confirmed** (ours); D1/IG byte **high** (spec), **not yet sent by us** |
| `0x28` | Outside temperature (as implemented) | len 12, all zero except `payload[5] = (t°C + 40) × 2`. Ex 21 °C → `2E 28 0C 00 00 00 00 00 7A 00 00 00 00 00 00 51` | 5 s | rav4 | **confirmed** (ours) — **CONFLICT** with spec, see next row |
| `0x28` | Air-conditioning info (official spec) | len 5. D0 `b7` A/C main, `b6` compressor, `b5` recirc, `b4` AUTO-hi, `b3` AUTO-lo, `b2` DUAL, `b1` MAX FRONT, `b0` REAR. D1 `b7` up vent, `b6` face, `b5` floor, `b4` display-request, `b3-0` fan 0-7. D2 driver setpoint, D3 passenger setpoint (`0x00` LO, `0x1F` HI, `0x20-0x23` = literal 16-19 °C, else LOWEST + (v-1)×0.5). D4 defog/AQS/ECO/units, `b0` 0 = °C / 1 = °F | on change | rav4 | **CONFLICT**: incompatible framings of the same byte. The 12-byte outside-temp form is what works on our unit — **keep it**. The 5-byte A/C form is the vendor spec and the RZC HU Java (which drops any 0x28 whose length ≠ 5). Do not try to send both. |
| `0x29` | Steering wheel angle | len 2. **Signed int16, whole degrees, LSB-first, sign-extended** — see §0.2. A 12-bit reader still decodes it correctly; a zero-extended 12-bit encoding does *not* survive an int16 reader. Range ±540 (spec says ±380; Clio III best at ±500). Positive = LEFT | 200-250 ms | rav4 | **confirmed** working (ours) — **CONFLICT on sign**: the leaked RZC HU Java reads the 12-bit value, **negates it**, then clamps ±380; the vendor spec just says >0 = left. If the reversing-camera guidelines bend the wrong way, emit `raw = (-angle) & 0x0FFF`. Also: the value on the wire must land in ±380 (spec) to ±540 (mikescotland, who notes "Clio III has best accuracy with −500..500"), **never ±5400**. |
| `0x20` | Steering-wheel keys | len 2. `[key code][state]`, state 0 = release, 1 = press, 2 = long-press. Full code table in §5 | event | rav4 | high (both source families agree on the table); **not implemented by any reference emitter — untested end to end** |
| `0x1E` | Rear parking radar + radar settings | len 5. `[RL][RLM][RRM][RR][settings]`. Settings: `b7` display on, `b6` distance near/far, `b5` **radar master enable**, `b2-0` volume 1-5 (spec) / `b3-0` volume (RZC Java). erik-rosen hardcodes `0xF7` | 100 ms / on change | rav4 | high. **Ordering dependency: the HU stores `b5` as `mRadarEnable` and discards every `0x1D` while it is clear — send `0x1E` before any `0x1D`.** |
| `0x1D` | Front parking radar | len 4. `[FR][FRM][FLM][FL]` — **right to left**, mirrored relative to `0x1E` | 100 ms / on change | rav4 | high (layout); distance scale = **CONFLICT**, below |
| — | *Radar distance scale* | **Spec: `0x00` = no display, `0x01` = nearest … `0x04` = farthest** (5 steps on Toyota). **smartgauges emits the inverse**: `v = max+1 − scale(dist)`, i.e. low = far, high = near, and with max=4 it can emit `5`, outside the spec range | — | rav4 | **CONFLICT — try the spec polarity first** (the vendor doc describes what the HU consumes; smartgauges is third-party box-side code). If bars fill when you back away, invert. |
| `0x21` | Trip info | len 7, **all big-endian**. `[avg speed BE16 ×0.1 km/h][elapsed BE16 in **MINUTES**][cruising range BE16 km][unit: 0 hide, 1 miles, 2 km]` | 5 s | rav4 | high. Both source families agree on MINUTES. canbox-nissan's `elapsed_sec` parameter feeds seconds into this field — **60× wrong, do not copy**. Spec says shown only when IG = ON. |
| `0x22` | Instantaneous fuel consumption | len 3. `[unit][value BE16 ×0.1]`; unit `0x00` MPG, `0x01` km/L, `0x02` L/100km. Ex 6.5 L/100km → `2E 22 03 02 00 41 97` | 1 s | rav4 | high (both families agree). Shown only when IG = ON. |
| `0x23` | History fuel consumption | **len 13.** `[unit][current-trip BE16][trip1..trip5 BE16]`, all ×0.1, `0xFFFF` = invalid. Slots fill **from the end** (one dataset → TRIP5) | 5 s | rav4 | high. canbox-nissan sends **len 3** here (copied from `0x22`) — that frame is dropped by the HU's exact-length guard. |
| `0x25` | TPMS | len 6. `[status][FL][FR][RL][RR][spare]`. Status `b7` TPMS present, `b6` alarm, `b5` show spare, `b2` layout (0 row / 1 car diagram), `b1-0` unit (00 = 0.1 BAR, 01 = PSI, 10 = 2.5 kPa). `0xFF` = no data. Ex `2E 25 06 A5 21 21 23 23 FF A8` | on change | rav4 | high (spec + RZC Java agree on length 6 and the gating `b7`). No sensors on our car. |
| `0x26` | Vehicle settings status | len 4. D0 lighting (`b7` DRL, `b6-4` headlamp sensitivity, `b3-2` interior-light delay, `b1-0` headlamp auto-off). D1/D2 door-lock settings. D3 A/C + **`b3-2` camera guideline style: 0 dynamic, 1 static, 2 warning lines** + relock timer | on change / on `0x90` | rav4 | high (spec) / medium (Java). **Never send steering angle on `0x26` — that is the VW mapping and it corrupts this menu.** |
| `0x27` | Past-15-minutes per-minute fuel | len 31. `[unit][15min BE16][14min]…[1min]`, each ×0.1, `0xFFFF` invalid | on change | rav4 | high (spec). If never received the HU simply hides the bar chart. |
| `0x30` | Version information | len 16, ASCII string (RZC Java accepts any length > 0 and decodes UTF-8) | power-on + on `0x90` | rav4 | high. **Best end-to-end liveness proof** — read it back in Factory Set → Display CANBUS Info. |
| `0x31` | Amplifier status | len 5. D0 `b7-4` FAD / `b3-0` BAL (0x00-0x0E). D1 `b7-4` BASS / `b3-0` TREBLE (−5..+5 = 0x02-0x0C, i.e. +7 offset). D2 `b7-4` MID / `b3-0` ASL (0x01 off, 0x08 on). D3 volume 0-63. D4 `b0` speed-dependent volume, `b1` beep | on change | rav4 | high (spec) / medium (Java) |
| `0x32` | System / node presence | len 1. `b0` OEM amp node, `b1` rear-seat node, `b2` 360 panoramic node, `b3` panoramic cut-in, `b6` amp power, `b7` mute | on change | rav4 | high (spec). If `b2` says panoramic exists the HU suppresses its own radar UI. |
| `0x50` | Vehicle-set status — exterior illumination | len 2. D0 `b1-0` off-delay: 0 OFF, 1 = 7.5 s, 2 = 15 s, 3 = 30 s. D1 reserved | on `0x90` | rav4 | medium (spec, added V1.19.004) |
| `0x51` | A/C panel keys | len 2. `[key][state]`; keys `0x87` PWR, `0x16` Enter, `0x81/0x82` VOL knob, `0x85/0x86` Tune knob. For **knobs** D1 = number of detents turned (1-255), not a press state | event | rav4 | medium — Crown-gated in the RZC Java, likely inert on a RAV4 profile |
| `0x52` | Vehicle-set return info | len 3. D0 `b7` rear-radar distance, D1 `b7` fuel unit (0 L/100km, 1 km/L), D2 `b2-0` colour theme | on change | rav4 | medium (spec name is verbatim `VECHILE`) |
| `0x58` | A/C info 2 — seat heat/vent | len 5. D0 `b7-4` driver seat heat / `b3-0` passenger. D1 passenger vents + display request + fan. D2 `b7-4` driver seat vent / `b3-0` passenger. D3/D4 reserved | on change | rav4 | medium — Crown-only |
| `0x1F` | Hybrid energy status | len 2. D0 `b7` = is-hybrid (gates everything), `b2-0` battery bars of 8. D1 flow bitmask: `b0` motor→batt, `b1` motor→wheels, `b2` engine→motor, `b3` engine→wheels, `b4` batt→motor, `b5` regen | on `0x90` | rav4 | high. **Do not emit** — see §8. |
| `0x03` | Parking brake / seatbelt / PRND | len 6, shape `2E 03 06 XX XX CS` — only two payload bytes ever described against a declared length of 6 | — | rav4 | **low / broken.** mikescotland verbatim: *"it's a mess for now from Raise — to be done once they sort it out."* Absent from the vendor spec. **Do not emit.** |

### 2b. VW-Polo profile — fallback only, listed because the command bytes **collide**

Do not mix these with the table above. Picking the wrong profile does not fail silently: it writes plausible garbage into the wrong subsystem.

| Cmd | Name | Payload layout | Interval | Profile | Confidence |
|---|---|---|---|---|---|
| `0x14` | Backlight / illumination | len 1. `0x00` darkest … `0xFF` brightest | power-on, on change, on `0x90` | vw | high (spec). **Semantic trap: `0x00` means the environment is BRIGHT** (key backlight off) so screen brightness should go high. Our Mégane source `0x645` b1 uses `0xFF` = off — opposite polarity, do not pass through raw. **No `0x14` exists in the Toyota profile at all**, and the RZC VW manager has no handler case for it. |
| `0x16` | Vehicle speed | len 2. `[LSB][MSB]`, km/h = LE u16 / 16 | on change | vw | high (both families). RZC VW manager only logs it — may be cosmetically dead. |
| `0x21` | Air conditioning | len 5. D0 `b7` A/C, `b6` compressor, `b5` recirc, `b4` AUTO-hi, `b3` AUTO-lo, `b2` DUAL, `b1` MAX FRONT, `b0` REAR. D1 `b7/b6/b5` vents, `b4` display request, `b3-0` fan 0-7. D2/D3 temps (`0x00` LO, `0x1F` HI, `0x01-0x11` = 18-26 °C in 0.5 steps per spec; smartgauges instead uses `b0` = half-degree flag, `b7-1` = integer °C). D4 `b7` AQS, `b5-4` left seat heat, `b1-0` right seat heat | 250 ms | vw | high (frame) / **CONFLICT on D2/D3 temp encoding** — try the spec's 0x01-0x11 table first; smartgauges' encoder is provably untested upstream (no car driver populates it). **Collides with Toyota `0x21` trip info.** |
| `0x22` | Rear radar | len 4. `[RL][RLM][RRM][RR]`, `0x00` no display, `0x01` nearest … `0x0A` farthest (11 steps) | on change | vw | high. **Collides with Toyota `0x22` instantaneous fuel.** Same scale-polarity conflict as §2a. |
| `0x23` | Front radar | len 4. `[FR][FRM][FLM][FL]` — right to left | on change | vw | high. **Collides with Toyota `0x23` history fuel.** |
| `0x24` | Basic vehicle info | len 1. `b0` reverse, `b1` handbrake/P, `b2` lights | 100 ms | vw | high. RZC VW manager logs it as *"base info (unused)"* and caches nothing — **reverse detection comes from `0x25` b1, not this**. (VW-MQB variant is instead a 1-byte door mask; BMW-NBT is 2 bytes.) |
| `0x25` | Parking-assist status | len 2. D0 `b0` radar tone, `b1` OPS/parking-assist on — **this is what fires `broadcastRadarPreview()`**. D1 reserved | 100 ms | vw | high. smartgauges sends this as **one** byte; the handler requires exactly 2, so it is dropped upstream — emit `{0x02, 0x00}`. **Collides with Toyota `0x25` TPMS.** |
| `0x26` | Steering wheel angle | len 2, **int16 little-endian**, D0 low byte. RZC Java negates it; no clamp | 250 ms | vw | high on LE (both the Java's sign test on D1 and smartgauges' `{ angle, angle>>8 }`). Range is per-car in smartgauges (±540 PQ, ±19980 MQB, ±5400 BMW). **Collides with Toyota `0x26` vehicle settings.** |
| `0x27` | Amplifier status | len 8. D0 amp type (0 none, 1 non-CAN, 0xFF unknown), D1 volume 0-30, D2 BALANCE −9..+9, D3 FADER, D4 BASS, D5 MID, D6 TREBLE, D7 speed-dependent volume 0-7 | on change | vw | high (spec) / low (RZC Java layout not recovered). **Collides with Toyota `0x27`.** |
| `0x30` | Version information | Layout not given in the VW doc; assume the Toyota 16-ASCII form | power-on / on `0x90` | vw | medium |
| `0x41` `01` | Body info — doors/safety | len 2. `[0x01][mask]`: `b0` LF door, `b1` RF door, `b2` LR door, `b3` rear/third door, `b4` trunk, `b5` handbrake **or bonnet** (smartgauges maps it to handbrake on Fabia/Q3/Premio/XC90, bonnet elsewhere), `b6` washer fluid low, `b7` driver seatbelt **NOT** fastened | 250 ms | vw | high. **`0x01` is a sub-command selector, not data.** |
| `0x41` `02` | Body info — vehicle block | len 13, **all big-endian**. `[0x02][RPM BE16 **raw, ×1**][speed BE16 ×0.01 km/h][battery BE16 ×0.01 V][outside temp BE16 ×0.1 °C signed, valid −50..+77.5][**trip** distance BE u24 km][remaining fuel, litres]` | 250 ms | vw | high — erik-rosen verified each field live (`01 FF` → 511 rpm; `01 FF` → 5 km/h; `00 1F` → 4 °C). **This is the only ambient-temperature field in the whole Raise protocol.** D9-D11 is **trip distance**, not odometer. smartgauges writes a boolean into the fuel byte — the fork fixes it. |
| `0x41` `03` | Body info — warnings | len 2. `[0x03][mask]`: `b7` low fuel, `b6` low battery voltage | on change only | vw | high |

---

## 3. HU → box messages

The two profiles are **disjoint**. A Toyota-profile HU sends only `0x81`, `0x83`, `0x84`, `0x90` — you will never see `0xC0`/`0xC2`/`0xC4` from it, so **volume arrives via `0x84` index 0x07**, not `0xC4`.

| Cmd | Name | Payload | Reply expected? | Profile | Confidence |
|---|---|---|---|---|---|
| `0x81` | Start / End communication | len 1. `0x01` = start (power-up), `0x00` = end (power-down) | **Spec: yes** — the connection is "established" once the box ACKs. In practice: no. smartgauges parses it and deliberately does nothing; canbox-nissan never looks at it and works | both | high |
| `0x90` | Request controller info | Payload bytes are the box→HU command ids the HU wants re-sent. **Toyota spec:** len 2, D0 ∈ {`0x1F` hybrid, `0x1E` rear radar, `0x1D` front radar}, D1 reserved. **RZC Java (HU side) polls a much wider set:** 0x28, 0x58, 0x21, 0x22, 0x23, 0x1F, 0x1D, 0x1E, 0x25, 0x24, 0x29, 0x30, 0x26, 0x50. **VW:** D0 ∈ {0x14, 0x16, 0x21, 0x24, 0x25, 0x26, 0x30, 0x41}, D1 = sub-command when D0 = 0x41 | **Yes — emit the named message immediately** | both | high. Decodes our own capture `2E 90 01 1E 50` = "send me rear radar" (checksum verifies). Note the DUDU sends **len 1** where the spec says len 2 — accept both. |
| `0x83` | Body / central-locking settings | len 2. `[index][value]`. Indices (spec, ~29): 0x00 autolock by speed, 0x01 autolock leaving P, 0x02 autounlock into P, 0x03 remote double-press, 0x04 DRL, 0x05 lock feedback tone, 0x06 headlamp sensitivity, 0x07 headlamp auto-off, 0x08 update history, 0x09 clear history, 0x0A clear 15-min fuel, 0x0B save settings, 0x0C illumination delay, 0x0D-0x13 smart-key / A/C linkage, 0x14 relock timer, **0x15 radar volume, 0x16 radar display on/off, 0x17 radar distance**, 0x20-0x22 panoramic + reversing-guideline settings, 0x23 tailgate opening, 0x24 fuel unit | Answer with `0x26` / `0x50` / `0x52`; safe to log-and-ignore initially | rav4 | high. Indices 1-9 and ≥12 only valid while IG = ON. |
| `0x84` | Amplifier / common settings | len 2. `[index][value]`. 0x01 FAD, 0x02 BAL (0x00-0x0E), 0x03 ASL, 0x04 BASS, 0x05 TREBLE, 0x06 MID (each −5..+5 = 0x02-0x0C, i.e. +7 offset), **0x07 VOLUME (0-63)**, 0x08 amp on/off, 0x09 surround, 0x0A mute, 0x0B beep | Optional `0x31` echo | rav4 | high. **Index 0x07 is the current HU volume — the single most useful inbound value on this profile.** |
| `0xA0` | Amplifier control | len 2. `[0x00 volume 0-30 (default 28) / 0x01 BAL / 0x02 FAD / 0x03 BASS / 0x04 MID / 0x05 TREBLE (each −9..+9) / 0x06 speed-dependent volume 0-7][value]` | No | vw | high |
| `0xA2` | Navigation info push | Layout **not recovered** from any source | Unknown | vw | **low** — but directly relevant to the AFFA3NAV turn-arrow goal. Capture live if the unit ever runs a VW profile. |
| `0xC0` | Source select | len 2. D0 source: 0x00 OFF, 0x01 Tuner, 0x02 Disc, 0x03 TV, 0x04 NAVI, 0x05 Phone, 0x06 iPod, 0x07 AUX, 0x08 USB, 0x09 SD, 0x0A DVB-T, 0x0B A2DP, 0x0C other, 0x0D CDC. D1 media type: 0x01 tuner, 0x10 simple audio, 0x11 enhanced audio, 0x12 iPod, 0x20 file video, 0x21 DVD, 0x22 other video, 0x30 AUX, 0x40 phone | No | vw | high. Our capture `2E C0 02 01 01 3B` verifies = Tuner/Tuner. |
| `0xC1` | Status-bar icon state | Layout not recovered | No | vw | medium on the name — **it is `HC_CMD_ICON`, not "play mode"** as our old notes said |
| `0xC2` | Radio / tuner | len 4. `[band: 0x00 FM / 0x10 AM][freq LE u16][0x00]` | No | vw | high. Our capture `2E C2 04 01 3C 28 00 D4` → `0x283C` = 10300 = 103.00 MHz (×0.01 MHz, **little-endian**). Note the observed band byte is `0x01`, not `0x00`. |
| `0xC3` | Media playback info | len 6, interpreted per current media type. Simple audio: `[disc][track][—][—][mm][ss]`. Enhanced audio: `[folder LE16][file LE16][mm][ss]`. **No text fields of any kind** | No | vw | high |
| `0xC4` | Volume | len 1. **CONFLICT:** RZC VW manager = 0-30 (`0x00-0x1E`) with `0x80` a discrete mute value; our older note said bit7 mute + 7-bit 0-127. Our capture `2E C4 01 04 36` (vol 4) fits both — **assume 0-30 + 0x80** | No | vw | medium |
| `0xC5` | Phone call status | Layout not recovered | No | vw | low |
| `0xC6` | Radar control / settings | Layout not recovered | No | vw | low |
| `0xA6` | *claimed* time sync | — | — | **none** | **refuted as documented.** Appears in no RZC constant table and in neither vendor spec; there is no date/time command anywhere in the Raise protocol. Its only trace is an empty `case 0xa6: //set time` in smartgauges — almost certainly the origin of the claim. Do not depend on it. |

### 3a. `0x9D` — THE HEAD UNIT'S CLOCK. Confirmed 2026-08-12

**`[hours][minutes][00]`, once a minute, unprompted.** In no vendor spec, in no
reference implementation, and in neither mikescotland PDF — and it contradicts
§3's own claim, repeated under `0xA6`, that *"there is no date/time command
anywhere in the Raise protocol"*. There is. It is inbound, and it is free.

Thirteen frames decoded against wall time, in capture order:

| Frame | Decodes as | | Frame | Decodes as |
|---|---|---|---|---|
| `10 39 00` | 16:57 | | `11 07 00` | 17:07 |
| `10 3A 00` | 16:58 | | `11 0A 00` | 17:10 |
| `10 3B 00` | 16:59 | | `11 19 00` | 17:25 |
| `11 00 00` | 17:00 | | `11 1A 00` | 17:26 |
| `11 01 00` | 17:01 | | `11 21 00` | 17:33 |
| `11 03 00` | 17:03 | | `11 22 00` | 17:34 |
| `11 04 00` | 17:04 | | `11 24/25/26 00` | 17:36, 17:37, 17:38 |

Every one a real minute, in order, matching the bench session's wall clock; the
`10`→`11` step is 16:59→17:00. The three consecutive frames at the end were
captured in a single 150-second window — one per minute, which is the cadence.

**WHAT THIS SUPERSEDES.** An earlier revision of this section read the first byte
as a "group" and the second as a source/app index, on the evidence that opening
Spotify produced `11 07 00`. That was a coincidence: Spotify was opened at 17:07.
The "missing `11 02`" it noted was simply a minute nobody captured. Both readings
are withdrawn.

**What it buys.** The panel's clock came from the PHONE over BLE, so a car with
no phone had no clock, and neither did a build without BLE. This
arrives on a wire that is already connected. `onHuClock()` in main.cpp consumes
it, validated (`h ≤ 23`, `m ≤ 59`) rather than trusted, because the command is
undocumented and another unit may put something else there.

### 3b. `0x99` — track title and artist. Confirmed 2026-08-12

The undocumented now-playing message this project has been hunting since M4b.
**`[0x83][field][UTF-16LE text, NUL-padded to a fixed 32 bytes]`**, payload
length 34.

| Field | Meaning | Samples |
|---|---|---|
| `0x02` | **title** | `Lost Control`, `Don't Wanna Die`, `Fale`, `W tych domach`, `Writing On The ` |
| `0x03` | **artist** | `BALZAM` (constant across four tracks), `Horror Vacui`, `Wieże Fabryk` |

Three things about it that are not guesses:

* **The text is UTF-16LE, not ASCII**, and non-Latin-1 code points really occur —
  `Wieże Fabryk` carries U+017C. Read a byte at a time it renders
  `.L.o.s.t. .C.o.n.t.r.o.l`, which is how the whole message sat in a log for a
  session looking like line noise.
* **The field is a fixed 32 bytes and the head unit sends at most 15 UTF-16 characters** (`k0.a3.c3` in the decompiled `com.syu.ms`, verified 2026-10-20; it was written as 16 here). Field 01 is binary, the track number and total (LE16, masked `& 0x12F` by the unit, so tracks from 16 up probably arrive mangled); 02 title, 03 artist, 04 album. Protocol 112 never forwards position, duration or play state, though the app has them.
  "Writing On The Walls" arrives as `Writing On The `. Nothing on our side can
  recover the rest — it is gone before the wire.
* **It fails `looksLikeText()`**, and any heuristic shaped like it: twelve
  printable bytes inside a 34-byte frame. Half the payload is NUL by
  construction, and the rest is padding. The marker must be tested first.

A third variant, `0x99` with a leading `0x20` and an otherwise empty payload,
precedes the two text frames on an **automatic** track change and is absent on a
manual one. It carries no text; treat it as a change marker and ignore it.

**No playback position anywhere.** Across ~100 captured frames nothing carries
elapsed or duration. The only other repeating inbound command is `0xC7` len 6,
whose byte 1 alternates `00`/`02` on a steady cadence — a state flag, not a
clock, because a clock would increment. Seeking on the phone produced no frame at
all. On this unit, "1:30 / 2:40" is not available.

### 3c. Superseded: `0x9D` as a source/app announcement

**In no vendor spec, in no reference implementation, and in neither mikescotland
PDF.** §3 above says a Toyota-profile head unit sends only `0x81`, `0x83`, `0x84`
and `0x90`; this one also sends `0x9D`, unprompted. It is the first evidence this
project has of the additive drift §0.6 describes — except on `0x9D` rather than
on `0x7D`, so the drift is not confined to the family everyone expected it in.

Every frame captured so far is **len 3**, and every checksum verifies:

| Frame | b0 | b1 | b2 | Seen when |
|---|---|---|---|---|
| `2E 9D 03 10 39 00 16` | `0x10` | `0x39` | 0 | idle, repeating |
| `2E 9D 03 10 3A 00 15` | `0x10` | `0x3A` | 0 | idle, repeating |
| `2E 9D 03 10 3B 00 14` | `0x10` | `0x3B` | 0 | idle, repeating |
| `2E 9D 03 11 00 00 4E` | `0x11` | `0x00` | 0 | idle, repeating |
| `2E 9D 03 11 01 00 4D` | `0x11` | `0x01` | 0 | idle, repeating |
| `2E 9D 03 11 03 00 4B` | `0x11` | `0x03` | 0 | idle, repeating |
| `2E 9D 03 11 04 00 4A` | `0x11` | `0x04` | 0 | idle, repeating |
| `2E 9D 03 11 07 00 47` | `0x11` | `0x07` | 0 | **opening Spotify** |
| `2E 9D 03 11 0A 00 44` | `0x11` | `0x0A` | 0 | received by us on the bench |

**Shape: `[group][index][value]`** — the same `[index][value]` pair `0x83` and
`0x84` use, with a page byte in front. Group `0x11`'s index tracks what the head
unit is *doing* (Spotify moved it to `0x07`), which makes group `0x11` look like
a source / active-app announcement and `0x10` like a second page of state.

**What is NOT established, and the third byte is why.** It reads `0x00` in every
sample taken so far, so `[group][index][value]` is indistinguishable from
`[page][item][state]` or from a two-byte identifier with a trailing constant.
Until a frame arrives with a non-zero third byte, the split above is a reading
and not a finding. Note also that `0x11 02` has never been seen — either the
index is skipped, or the capture missed it.

**It costs us nothing and requires nothing.** It is inbound only; emulating a
canbox does not involve sending it, and `RxParser` already validates it, records
it and answers the bare `0xFF` ACK like any other frame. Log and ignore.

**How to finish it:** the console echo (`huDrainSerial`) prints every frame with its ASCII column, so the map falls out of a
labelled session — open each source in turn, note which index follows. That is
twenty minutes with a head unit on a desk and is not reachable any other way,
because no document describes this command.

---

## 4. Initialisation, ACK, keepalive

**There is no initialisation.** No handshake, no boot burst, no keepalive, no connection state machine. smartgauges emits telemetry unconditionally from power-on; canbox-nissan's "handshake()" is literally `while (RadioSerial.available()) RadioSerial.read();` — drain and discard, once per loop pass. Our own 7 working messages are the local proof: they display without any connect sequence. Spec-wise the HU is supposed to open with `0x81 {0x01}` and re-issue it after every ACC cycle; recognise it, never gate on it.

**ACK.** The official Chinese doc defines four response bytes — `0xFF` ACK, `0xF0` NACK (bad checksum), `0xF3` NACK (command not supported), `0xFC` NACK (busy) — with a 10 ms reply deadline, a 0-100 ms receive window, retransmit after 100 ms of silence, and a hard stop after 3 failures. smartgauges writes a single **bare `0xFF` byte** (not a `0x2E` frame) the instant it receives the last byte of any inbound frame, before dispatching it. Recommended for us: **send one bare `0xFF` per received frame, consume and log inbound ACKs, never block, and never implement the 3-strikes stop** — it would silence a working link on a head unit that simply does not ACK. `0xF3` is worth logging loudly: it is the HU telling us a message is not in its profile.

**Do keys need anything first? Unresolved — this is the biggest open question in the document.** Neither reference emitter (canbox-nissan is telemetry-only; smartgauges targets a different HU family) proves `0x20` on a DUDU. Nothing in either vendor spec or the RZC Java gates `0x20` on any prior message. Two things *might* gate it and both are cheap to rule out:

- *Bench experiment A (keys):* with the unit on Raise/Toyota RAV4 and the media screen open, send `2E 20 02 01 01 DB` (VOL+ press) followed immediately by `2E 20 02 01 00 DC` (release), no delay between them, five times at 1 s intervals. Volume should step. If nothing happens, repeat with Factory Set → password 108 → Display CANBUS Info open to confirm the bytes are arriving at all. If the bytes arrive and nothing moves, try `0x02` (VOL−) and `0x07` (SRC) before concluding the key path is dead — a single wrong code proves nothing.
- *Bench experiment B (the IG gate):* the spec states repeatedly that trip (`0x21`), instantaneous fuel (`0x22`), history fuel (`0x23`) and TPMS (`0x25`) are only displayed 只有当IG点火开关为ON. We currently send `0x24` with **len 1**, so the IG bit is never asserted. Send `2E 24 02 00 02 D7` (len 2, no doors open, D1 `b1` = IG ON) at 250 ms, then send a `0x21` trip frame and see whether the trip panel populates. **This is the highest-value single experiment in the whole list** — it may be the difference between "our info panels are blank" and "our info panels work", at the cost of one byte.
- *Prerequisite for both, 30 seconds:* send `0x30` with a 16-byte ASCII version string and read it back in Display CANBUS Info. If that does not appear, the link is down and every other result is meaningless.

---

## 5. RAV4 / Toyota steering-wheel key codes (`0x20`, `[code][state]`)

State: `0` release, `1` press, `2` long press. Both source families (official Chinese spec table and the leaked `RZC_ToyotaSeriesProtocol.java` `SWC_KEY_*` / `SCROLL_KEY_*` constants) give the identical list.

| Code | Function | Code | Function |
|---|---|---|---|
| `0x00` | NONE / released | `0x16` | ENTER / OK |
| `0x01` | VOLUME UP | `0x81` | scroll knob: volume up |
| `0x02` | VOLUME DOWN | `0x82` | scroll knob: volume down |
| `0x03` | MENU RIGHT | `0x83` | scroll knob: CH/FLD up |
| `0x04` | MENU LEFT | `0x84` | scroll knob: CH/FLD down |
| `0x07` | SRC / MODE (hold = mute) | `0x85` | scroll knob: TUNE/TRACK up |
| `0x08` | SPEECH / voice | `0x86` | scroll knob: TUNE/TRACK down |
| `0x09` | PICK UP (answer call) | `0x87` | POWER |
| `0x0A` | HANG UP | `0x88` | **MODE** |
| `0x0B` | BOX key | | |
| `0x13` | MENU UP | | |
| `0x14` | MENU DOWN | | |
| `0x15` | BACK | | |

Confidence **high** for the table. Two corrections to our older notes: **`0x88` is MODE, not TEL** — the telephone keys are `0x09`/`0x0A`; and codes `0x03/0x04/0x0A/0x0B/0x13/0x14/0x15/0x81-0x84/0x87` were simply missing.

**~~CONFLICT~~ RESOLVED on `0x85`/`0x86` — confirmed on our own DUDU7, 2026-08-12.** The constant names say `SCROLL_TUNE_TRACK_UP` = `0x85` / `..._DOWN` = `0x86`; erik-rosen's on-car test found the opposite. **erik-rosen is right and the constant names are misleading.** Swept on the bench:

| Result | Codes that produce it |
|---|---|
| **next track** | `0x03` MENU RIGHT, `0x83` CH/FLD up, `0x86` |
| **previous track** | `0x04` MENU LEFT, `0x84` CH/FLD down, `0x85` |

Three codes per direction, all equivalent on this unit — so transport is reachable three ways and the choice is free. Use `0x86`/`0x85`.

**Also confirmed in the same sweep, and two of them settle open questions:**

| Code | Behaviour on our DUDU7 |
|---|---|
| `0x01` / `0x02` | volume up / down, one step |
| `0x81` / `0x82` | the **knob** form — runs continuously toward max/min rather than stepping once. Byte 2 is a detent count, so vary *that*, not the repeat rate |
| `0x07` | **opens the source-select page.** It is not mute, and holding it is not mute either — see §12 open question 4, which this half-answers: mute is *not* long-SOURCE on this unit, and no dedicated mute code has been found yet |
| `0x15` | BACK — works as named |
| `0x13` / `0x14` | MENU UP / DOWN — behave identically to `0x86`/`0x85` here |
| `0x08` | voice assistant — works |
| `0x09` / `0x0A` | answer / hang up — `0x09` works |
| `0x88` | MODE — opens a vehicle-dashboard menu |
| — | **no play/pause code exists in this profile.** Bind a spare code to Play/Pause on the unit's own Steering Wheel Learning page instead |

For reference, the VW-profile key set is much smaller and does not overlap in meaning: `0x01` VOL+, `0x02` VOL−, `0x03` menu up, `0x04` menu down, `0x05` TEL, `0x06` MUTE, `0x07` SRC, `0x08` MIC (smartgauges additionally emits undeclared `0x09` resume/pause, `0x0A` app switch, `0x0B` mute toggle, `0x0C` nothing; PQ and MQB swap prev/next).

---

## 6. Renault Mégane II CAN IDs

Bus topology (workshop manual §88B): three links bridged by the UCH — engine/powertrain, comfort/body, multimedia.

### Confirmed (measured on our own vehicle or by a Mégane II-specific source)

| Item | Value | Source | Vehicle the source actually covers |
|---|---|---|---|
| Multimedia SWC keys | `0x0A9` (UpdateList) / `0x1C1` (Carminat), frame `03 89 <hi> <lo>`, hold = `0x80`/`0x40`; 0 LOAD, 1 SRC→, 2 SRC←, 3 VOL+, 4 VOL−, 5 PAUSE, `0x0101` SEEK+, `0x0141` SEEK− | Our own firmware (`AffaKey`) running in the car | **Mégane II Ph1 CC 2004 — ours** |
| AFFA3 display protocol | Carminat sync `0x3AF` / reply `0x3CF`, ctrl+text `0x151`; UpdateList sync `0x3DF` / reply `0x3CF`, ctrl `0x1B1`, text `0x121` | Our own firmware, in service daily | **Mégane II Ph1 CC 2004 — ours** |
| OBD-II pins 6/14 = main CAN @ 500 kbit/s | — | Racelogic "Renault Mégane II (2002-2010)" PDF | **Mégane II 2002-2010 — exactly ours** |
| Signals present on that OBD bus | brake, coolant temp, doors FL/FR, RPM, fuel consumption, full beam, handbrake, headlights, ignition acc/on/starter, speed, indicators L/R, side lights, steering angle, throttle, 4× wheel speed, wipers | Racelogic, same PDF | **Mégane II 2002-2010.** Confirms the *signals exist*; it does **not** give IDs or bit positions. |
| Bus partially alive at key-off | `0x354`, `0x5C5`, `0x5FD`, `0x60D`, `0x715` keep transmitting | our own passive observation | Mégane II Ph1 CC 2004 — ours |

### Probable (correct-era Renault, IDs not yet verified on our car — P2 correlation sniff is mandatory)

All of the following are from **x0r.fr/blog/39, which decodes a Clio III**, same platform generation and same frame conventions (checksum = 8-bit sum of the other bytes, one's-complemented); partially corroborated on a Mégane II Ph2 by forum user *frkgnn*. Confidence **medium** for every row.

| ID | Content | Notes |
|---|---|---|
| `0x0C2` | steering angle b0-1 (−0x8000 bias, 0.1°/LSB), rotation speed b2-3 | primary steering source for `0x29` |
| `0x181` | RPM b0-1 (**/8**), accel pedal b3, b5 bit3 clutch / bit0 brake | divisor 8 independently derived twice in the x0r comment thread from known idle/cruise RPM and confirmed against ELM |
| `0x1F9` | RPM b2-3 (/8) | second RPM source |
| `0x215` | b1 bit6 reverse gear | |
| `0x354` | speed b0-1 ×0.01 km/h, distance b2-3 ×0.1, b4 bit4 brake pedal | ×0.01 matches `0x7D 03`'s wire scaling exactly — pass through with no arithmetic |
| `0x551` | b0 engine temp −40, b1.. fuel used (12500 = 1 L) | |
| `0x5C5` | d0 bit2 handbrake (0x04), d0 bit5 low fuel, d1-d3 odometer km, d7 & 0x7F fuel litres | **Handbrake conflict settled by the X84 matrix: bit2** (mikescotland), not x0r's bit3; decoded since 2026-10-20 as a hypothesis row. |
| `0x5FD` | b0-2 odometer, b3-5 vehicle age in minutes | feeds `0x7D 04` |
| `0x60D` | b0-2 status bits: 23 boot, 22 RR door, 21 RL, 20 FR, 19 FL, 18 position lights, 17 dipped, 14 right ind, 13 left ind, 11 main beam, 10 key-on, 9 key-acc, 8 front fog, 5 doors locked, 4 boot locked, 2 rear fog; b4 outside temp −40; b5 engine temp −40; b6 bit4 reverse; b7 bits0-1 right stalk | **the single richest frame** — feeds `0x7D 01` lights, `0x24` doors, `0x28` temp, and the IG bit all at once |
| `0x645` | b1 instrument backlight `0x00-0xFE` (**`0xFF` = off**), b3-4 speed | illumination source; note the polarity inversion vs Raise `0x14` |
| `0x651` | b0 bit1 passenger airbag off / bit2 key acc-run; b1 bit0 driver belt | |

### Weak / not applicable — other Renault platforms, IDs **not** transferable

| ID | Claim | Source vehicle | Verdict |
|---|---|---|---|
| `0x186` | RPM (b1, ≈raw×32) | Kaptur / Duster, CAN-Hacker | **contradicted** by the next row — cannot both be right |
| `0x186` | throttle, bit47 len10 ×0.1 | Mégane 3 RS DBC (krsche) | **contradicted** — IDs shift between platforms; treat `0x186` as unusable until sniffed |
| `0x0C6` | steering angle b0-1 ×0.1, −3276.7° bias | Mégane 3 RS DBC | plausible but a different generation than `0x0C2` above; sniff both |
| `0x217` / `0x3F7` / `0x5DE` | speed / PRNDL / turn lamps | Kaptur | different platform, low confidence |
| `0x29A`, `0x29C`, `0x1F6`, `0x352`, `0x4F8` | wheel speeds ×0.005, brake, brake pressure, handbrake — **big-endian** | Mégane 3 RS DBC | different generation *and* different endianness convention; do not port blind |

### Refuted / dead ends on the car side

- **Fuel level IS broadcast (2026-10-20, from Renault's X84 broadcast matrix; not yet measured on our car):** `0x5C5` d7 & 0x7F = litres (the cluster's), with the low-fuel warning in d0 bit5; `0x715` d4 >> 1 = litres (its after-sales backup copy). x0r's 0x5C5 sample gives 19 L. Decoded as `fuel_level`, `fuel_low`, `fuel_level_apv` (decode_defaults.inc, hypothesis rows). The fuel needle cannot be moved from the bus: the cluster drives it from the sender wire. On the Toyota profile `0x41` carries no fuel field (below), so the head unit has no place for it except the experiments in hu-decoder.md §2b.
- **Pre-≈2005 analogue SWC/display path** — a real concern for early Mégane II, refuted for ours: the AFFA3 CAN firmware works, so our car is CAN-based.
- **`0x186` as a stable ID** — see above.

---

## 7. Implement next (ordered)

> **Status.** Items 1-10 and 12 are implemented; the whole documented RAV4 box→HU
> set except `0x03`, `0x27` and `0x2b`-family gaps is now composable and sendable
> from the Сервіс screen (`/api/messages`), with a preview of the exact bytes and a raw sender for anything
> not in the table. What is left is **bench work, not code**: items 3, 11 and 13
> are questions only the head unit can answer, and §4's two experiments are now
> one checkbox and one button each.



1. **`0x24` → len 2 with D1 bit1 = IG ON** — one extra byte; per spec it is what gates the trip, fuel, history and TPMS panels from displaying at all.
2. **Fix `0x24` door handedness for LHD** — driver = **left** front = `b6` (`0x40`), passenger = `b7` (`0x80`); canbox-nissan's `MASK_DOOR_DRIVER = 0x80` is RHD (mikescotland's car) and swaps our icons.
3. **`0x20` steering-wheel keys** — the actual point of the box; the whole SWC→GW→HU path is unexercised and everything else is cosmetic next to it.
4. **`0x30` version string (16 ASCII)** — 20 lines of code, gives a permanent one-glance liveness check in Display CANBUS Info and makes every future bench test interpretable.
5. **`0x90` responder** — parse the payload as a command id and emit that message immediately; this is the cleanest way to make the DUDU *tell us* which messages its profile actually wants, instead of guessing.
6. **`0x84` index 0x07 inbound** — current HU volume, free, and the only useful inbound value on the Toyota profile.
7. **`0x22` instantaneous fuel (len 3, BE)** — `0x551` gives us fuel-used; cheap, and validates the IG gate from item 1.
8. **`0x21` trip info (len 7, BE, elapsed in MINUTES)** — reuse the same sources; do not copy canbox-nissan's seconds bug.
9. **Bare `0xFF` ACK on every received frame** — costs one `write()`, matches both the spec and smartgauges, and removes a whole class of "the HU went quiet" mysteries.
10. **`0x23` history fuel at the full len 13** — the only way the HISTORY screen ever populates; the len-3 form everyone ships is silently dropped.
11. **`0x1E` rear radar then `0x1D` front** *(only if the car has park sensors)* — and always `0x1E` first with the control byte's `b5` set, or every `0x1D` is discarded.
12. **`0x32` system info (len 1)** — one byte that tells the HU which UI affordances to draw; cheap and prevents phantom widgets.
13. **`0x29` sign check** — a two-minute bench test (turn left, watch the guidelines) that either confirms the current output or tells us to emit `(-angle) & 0x0FFF`.

---

## 8. Do not implement

- **`0x03` (parking brake / seatbelt / PRND)** — declared len 6 with two described bytes, absent from the vendor spec, and its author says it is unfinished. This is the concrete identity of the "spurious gearbox icons / seatbelt always on" DUDU quirk. Skip the whole address.
- **`0x1F` hybrid energy** — do *not* emit even with zeros or a clear `b7`; a malformed one risks the HU rendering a hybrid widget on a Mégane. Leaving it unsent is the correct behaviour.
- **`0x25` TPMS, `0x27` 15-minute fuel** — no sensor and no per-minute history on our car; sending zeros is worse than sending nothing (the HU hides the panels when the frame never arrives).
- **`0x51`, `0x52`, `0x58`** — Crown/Highlander-gated in the RZC Java; inert on a RAV4 profile.
- **`0x26` as steering angle** — that is the *VW* mapping. On the Toyota profile `0x26` is the vehicle-settings block; sending angle there corrupts the settings menu.
- **`0x14` illumination on the Toyota profile** — the command does not exist there. It is VW-only, and even in the VW RZC manager there is no handler case for it. smartgauges implements illumination as a GPIO output and never transmits it. (If we ever do use it: `0x00` = bright environment, and our `0x645` uses the opposite polarity.)
- **`0x41` anything on the Toyota profile** — absent from the Toyota spec; the head unit's k0.a3 does decode it, but into the same Toyota codes as `0x7D` (body bits, odometer/trips, 154/155), not the VW battery/fuel/belt fields (hu-decoder.md §2c). Nothing to gain.
- **`0xA6` time sync** — not in any constant table in either direction, in either profile. The claim traces to an empty `case 0xa6: //set time` in smartgauges. Do not build clock sync on it.
- **Expecting `0xC0`/`0xC2`/`0xC3`/`0xC4` from a Toyota-profile HU** — the entire HU→box set there is `0x81`/`0x83`/`0x84`/`0x90`. Volume comes from `0x84` idx 0x07.
- ~~**Waiting for track title/artist over Raise**~~ — **THIS ROW IS WRONG, corrected by
  hand after review.** It is a statement about the *specification*, presented as one about
  the wire. A DUDU owner on the vendor forum (mikescotland) drove an LCD from the head
  unit's UART and showed **title and artist on track change** — and the previous project's
  own notes record it (`ARCHITECTURE-V2.md:439`, `:31`). The precedent was already in
  evidence: the entire `0x7D` family appears in no vendor document either, and is
  demonstrably real. Undocumented is not absent. The firmware now buffers inbound frames
  up to 192 bytes, verifies the checksum with a running sum so an over-long frame is
  still validated rather than dropped, consumes the declared length so an `0x2E` inside
  text cannot resynchronise the parser, and renders printable payloads as ASCII in the
  web log. Original text follows for the record:
- **Waiting for track title/artist over Raise** — there is no string field anywhere in either protocol except `0x30`. `0xC3` is `[total LE16][current LE16][mm][ss]` and is VW-only besides. **Promote the DUDU-side Android `NotificationListenerService` app from Plan B to the primary path for media text.**
- **The 3-strikes "stop all transmission" retransmit rule from the spec** — it would silence a working link on a head unit that simply does not ACK (which is exactly how the DUDU behaves for canbox-nissan).
- **Inheriting these four known upstream bugs:** smartgauges' 1-byte VW `0x25` (handler needs 2, frame dropped, radar preview never fires); erik-rosen's 1-byte Toyota `0x24` (needs 2, dropped); canbox-nissan's len-3 `0x23`; canbox-nissan's `elapsed_sec` into `0x21`'s minutes field.
- **`RadioSend_initial.cpp`'s big-endian RPM on `0x26`** — a discarded draft in canbox-nissan's history, not a third opinion. On either profile that command byte means something destructive.
- **Any single shared command table across profiles.** The collisions are `0x21`, `0x22`, `0x23`, `0x25`, `0x26`, `0x27` — every one of them plausible-looking garbage in the wrong subsystem rather than a clean failure. Make the profile a compile- or config-time selector with **no shared command constants**.

---

## Unreachable / incomplete sources

- ~~**mikescotland's Google Drive PDFs were not fetched directly**~~ — **CLOSED, see §0.** Both were retrieved and read in full; nine of their eleven worked examples verify, and the two that do not are arithmetic errors in the document, not a second framing rule.
- **xdaforums.com/t/…4057759** ("protocol bible") — returned HTTP 403 on every direct fetch, main page and page 2. One page-4 post (#67) was recovered indirectly and is quoted in the RPM analysis: a 2015 Passat HU log showing `2e 7d 03 0b 70 30 d4` / `2e 7d 03 0b 3e 30 06`, both checksums valid, decoding as battery millivolts — independent evidence that the `0x7D` family and its little-endian sub-command convention are genuine shipping Raise firmware. The rest of the thread's HU→box tables were not read.
- **`0x7D` has no vendor specification anywhere.** Grepping the official Ruizhicheng CAMRY/RAV4 V2.21.000 PDF for `0x7D`, RPM, 转速 and 发动机 returns nothing; there is no engine-speed, odometer or lights message in the Toyota spec at all. The entire `0x7D` lineage is **one document (mikescotland) plus one implementation of that document (canbox-nissan)** — treat them as a single source, not two. There will never be vendor errata for it; the DUDU is the only authority.
- **`0xA2` HC_CMD_NAVI_INFO (VW)** — layout not recovered from the RZC manager or either spec. Relevant to the AFFA3NAV turn-arrow goal; would need a live capture.
- **VW `0xC1`, `0xC5`, `0xC6`, and `0x27`'s RZC-side handler** — constants declared, no parser recovered.
- **VW `0x14`** — constant declared in `RZC_VolkswagenSeriesProtocol`, **no handler case found** in the manager; layout comes from the VW spec document only.
- **VW `0x30`** — listed in the DataType table; the VW document gives no byte layout.
- **`0xC3` iPod / video sub-layouts** — the per-media-type interpretation table was only partially recovered.
- ~~**mikescotland's Google Drive PDFs** were not fetched directly~~ — superseded, see §0. The vendored copies in `aerodomigue/esp32-canbox-nissan/docs/protocols/` were what earlier revisions of this document were built from; the originals now agree with them, and add the two corrections in §0.2 and §0.3.