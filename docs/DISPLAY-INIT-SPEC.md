# Carminat display init — the authentication sequence, step by step

**Purpose:** every step of bringing the Carminat panel up, with the exact bytes, so each
step can be confirmed on the wire independently instead of judged by whether text appears.

**Method:** read out of MeganeCAN (`src/display/Carminat/CarminatDisplay.cpp`,
`CarminatConstants.h`, `AffaDisplayBase.cpp`) and out of AffaDisplay
(`src/carminat/CarminatConstants.h`, `src/core/AffaDisplayBase.cpp`), then compared byte by
byte. Where they differ, it is called out.

---

## 0. Headline result of the comparison

**The authentication is already byte-identical to MeganeCAN.** Every constant, every frame,
every ordering. The table in §6 is the audit.

There is **exactly one behavioural difference**, and it is deliberate on the library's side:
MeganeCAN sends an extra heartbeat *in reply to* the panel's ping; the library does not.
See §5. That is the only candidate if a "same auth as MeganeCAN" change is wanted.

---

## 1. Identifiers

| Name | ID | Direction | Purpose |
|---|---|---|---|
| `SYNC` | `0x3AF` | **we transmit** | heartbeat, sync request, hello |
| `SYNC_REPLY` | `0x3CF` | panel transmits | registration request, ping |
| `SETTEXT` / `DISPLAY_CTRL` | `0x151` | **we transmit** | all screen payloads |
| `NAV` | `0x1F1` | **we transmit** | second registered function |
| `KEYPRESSED` | `0x1C1` | panel transmits | stalk keys |
| ACK channel | `id \| 0x400` | panel transmits | `0x551` answers `0x151`, `0x5F1` answers `0x1F1` |

Filler byte: **`0x00`** (both projects). Bitrate 500 kbit/s, standard 11-bit IDs.

---

## 2. The steady state — heartbeat, at 1 Hz

Unconditionally, once per second, whatever else is happening:

```
T 3AF   B9 00 00 00 00 00 00 00        "I am alive"
```

`0xB9` is the alive byte. The seven bytes after it are filler that happens to be zero.

**Verify:** `GET /api/can/last` shows `3AF B900000000000000` at roughly 1/s. If it appears
hundreds of times a second, the pacing is broken — that was the original 2026-07-27 bug.

---

## 3. Requesting sync

Sent in the same 1 Hz tick, but **only** while `FAILED` or `START` is set:

```
T 3AF   BA 00 00 00 00 00 00 00        "please resynchronise"
```

`0xBA` = request. **The second byte is filler, not an argument** — this matters because
UpdateList's equivalent (`7A 01`) *does* carry a real argument, and the two look
symmetrical on the wire. The library marks this `[CAP]` and says it must stay `0x00`.

---

## 4. The handshake proper

### Step 1 — the panel asks

```
R 3CF   61 11 00 A3 A3 A3 A3 A3        panel: "register with me"
```

Byte 2 (`data[2]`) is a flag: `0x01` means the panel wants a full restart, and sets `START`
so §3's `BA` goes out on the next tick.

### Step 2 — we answer with three frames, in this order

```
T 3AF   70 1A 11 00 00 00 00 01
T 3AF   B0 14 11 00 1F 00 00 00
T 3AF   B0 14 11 00 1F 00 00 00        <- IDENTICAL to the previous line, on purpose
```

**The third frame is a deliberate duplicate.** Two `sendCan` calls in MeganeCAN, present in
the OEM capture, and the library reproduces it and marks it `[CAP]` — *"It is not a typo and
it is not deduplicated."* Anything that "cleans this up" breaks the handshake.

`FAILED` is cleared here. **Registration is not yet complete.**

### Step 3 — the panel pings

```
R 3CF   69 00 A3 A3 A3 A3 A3 A3        panel: "still here"
```

Roughly 1/s. Credits `PEER_ALIVE`. See §5 — this is the one place the two implementations
differ.

### Step 4 — function registration, lazily, on the first render

This is the step that actually latches `FUNCSREG`, and the step that fails silently.

Before the first payload after a resync, a **one-byte `0x70`** is put on each function ID in
declaration order, and **each must be acknowledged on `id | 0x400`**:

```
T 151   70 00 00 00 00 00 00 00
R 551   74 …                           DONE  -> 0x151 registered
T 1F1   70 00 00 00 00 00 00 00
R 5F1   74 …                           DONE  -> 0x1F1 registered
```

Both projects register **`{0x151, 0x1F1}`**, in that order. Only after *both* ACK does
`FUNCSREG` latch. MeganeCAN returns an error from `affa3_send` and leaves `FUNCSREG` clear if
either probe times out; the library does the equivalent.

**This is the most common place to stall,** and its signature is distinctive: the `T 151 70`
goes out and **no `0x551` ever comes back**. Seen on 2026-07-28 — the panel looped §4 step 1
forever while never answering the probe. It cleared on its own.

### Step 5 — payload

Only now do screens go out, ISO-TP framed on `0x151`, with the panel flow-controlling
`30 01 00` per frame and `74` at the end.

```
T 151   10 60 21 05 FF 00 00 40        first frame, declared length 0x60
R 551   30 01 00 …                     ContinueToSend, BS=1, STmin=0
T 151   21 …                           continuations
…
T 151   2D …                           last frame of a 14-frame fullscreen
R 551   74 …                           DONE
```

---

## 5. THE ONE DIFFERENCE

**MeganeCAN**, `CarminatDisplay.cpp:346`:

```cpp
else if (packet->data.uint8[0] == 0x69)
{
  _sync_status |= SyncStatus::PEER_ALIVE;
  tick();                     // <-- sends B9 IMMEDIATELY, in reply to the ping
}
```

**AffaDisplay**, `core/AffaDisplayBase.cpp:292`:

```cpp
if (f.data[0] == kSyncPeerAlive) {
  _sync |= SyncState::PeerAlive;   // set the flag and nothing else
  return true;
}
```

with the comment:

> The legacy code called `tick()` from here, which emitted an extra heartbeat per ping and
> re-armed the watchdog from the PEER rather than from the loop. Both are gone: exactly one
> heartbeat leaves per `AFFA_SYNC_INTERVAL_MS`, paced by `pumpSync()`.

### What this changes on the wire

| | MeganeCAN | AffaDisplay |
|---|---|---|
| Heartbeats per second | **two** — one on the tick, one triggered by the panel's ping | one |
| Relationship to the ping | `B9` follows `69` within milliseconds — reads as a **reply** | `B9` is on its own clock, unrelated to when `69` arrived |
| Watchdog re-armed from | the peer's ping | our own loop |

The library's reasoning is sound in isolation: one heartbeat per interval, paced by a
wall clock. **But it is a protocol change against a peer nobody has a spec for.** If the
panel treats our `B9` as an answer to its `69`, the library never answers — it just happens
to transmit nearby.

**This is the only thing to change if "exact same auth as MeganeCAN" is wanted**, and it is
testable in isolation: emit a heartbeat immediately on receiving `0x69`, in addition to the
paced one, and watch whether §4 step 4 stops stalling.

Doing that in this repository would be a workaround; per the standing instruction it belongs
in the library, behind a profile flag (e.g. `SyncProfile::replyToPing`), so the Carminat
profile can have MeganeCAN's behaviour and UpdateList keep the paced one.

---

## 6. Byte-level audit

| Item | MeganeCAN | AffaDisplay | |
|---|---|---|---|
| Sync TX id | `0x3AF` | `0x3AF` | same |
| Sync RX id | `0x3CF` | `0x3CF` | same |
| ACK offset | `+0x400` | `0x0400` | same |
| Alive byte | `0xB9` | `0xB9` | same |
| Request byte | `0xBA` | `0xBA` | same |
| Request arg | filler `0x00` | `0x00` | same |
| Filler | `0x00` | `0x00` | same |
| Hello frame 1 | `70 1A 11 00 00 00 00 01` | identical | same |
| Hello frame 2 | `B0 14 11 00 1F 00 00 00` | identical | same |
| Hello frame 3 | `B0 14 11 00 1F 00 00 00` (dup) | identical, dup preserved | same |
| `START` from | `data[2] == 0x01` | `data[2] == 0x01` | same |
| Function IDs | `{0x151, 0x1F1}` | `{0x151, 0x1F1}` | same |
| Probe payload | one byte `0x70` | one byte `0x70` | same |
| `FUNCSREG` needs | every probe ACKed | every probe ACKed | same |
| Heartbeat on `0x69` | **yes, immediate** | **no** | **DIFFERS — §5** |
| Peer timeout | `SYNC_TIMEOUT` ticks | `AFFA_PEER_TIMEOUT_MS` wall clock | equivalent-but-better; the tick version broke when `tick()` was called at loop rate |

---

## 7. Failure signatures

Match the symptom to the step rather than guessing.

| What the wire shows | Step | Meaning |
|---|---|---|
| No `3AF B9` at all | §2 | we are not transmitting: check `listenOnly`, the TX gate, and `canWiring` in `/api/settings` |
| `3AF B9` hundreds/second | §2 | pacing broken |
| `R 3CF 61 11` repeating, we answer each time | §4.1–2 | panel is not progressing past the hello — go to the next row |
| `T 151 70` with **no `R 551`** | §4.4 | **registration stalled.** The panel is not acknowledging the probe. This is the 2026-07-28 stall |
| `30 01 00` after every frame but never `74` | §4.5 | transfer completes on the wire, no terminal ack — every render reports `Timeout` |
| Everything acked, `Ok`, nothing on the glass | — | the display is powered off: send `setPower(true)`, and note it needs its **delivery** verdict checked, not its acceptance verdict |
| `sync 0x08` vs `0x0A` | — | **not a fault.** `PEER_ALIVE` toggles at 1 Hz; both values are healthy. Use `registered` |

---

## 8. How to verify, in order

Nothing here needs the panel to display anything — each step is confirmable on the wire.

1. `GET /api/can` — `state: running`, `busErr` not climbing, `pins` = `rx 4 / tx 3`.
2. `GET /api/can/last` — `3AF B9…` at ~1/s, and `3CF 69…` coming back at ~1/s. That proves
   both directions before any screen is involved.
3. `GET /api/display` — `registered: true`. If false, look for §7's `T 151 70` row.
4. `POST /api/display/text?text=SUCCESS` — then read the glass. This is the one step that
   needs a human.
5. Only then turn the media screen on.

---

## 9. Open, and honest

* The 2026-07-28 registration stall (§4 step 4) **was never explained.** It cleared on its
  own and the link then ran 41 minutes with zero failures. §5 is the best candidate.
* The residual `36 / 3293` render failures all occurred inside the first six minutes after a
  boot. Also unexplained.
* §5's difference is **untested** — it is a code comparison, not a measurement. It should be
  tried as a library profile flag and confirmed on the wire before anyone believes it.

---

## 10. Bench facts about this panel

* Stalk keys arrive on `0x1C1` as `02 64 0F ...`; the panel never puts `0x70` on it.
* Before registration the only id it sends is `3CF 61 11 01`, at ~1570 frames/s.
* Fullscreen (`21 05`): header byte 5 is the body length (declLen − 0x20); text is read
  from content byte 32; ~22 wide characters per row. The OEM pair 0x69/0x49 shows three
  full rows (camera, 2026-09-27; `MegaOpen::sendFullscreen`).
* Popup/setText icon byte: inverted mask; 0x55 = no icons, 0x09 lights i(TRAFFIC),
  AF-RDS and LIST (camera, 2026-09-27).
