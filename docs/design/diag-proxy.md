# Diagnostic proxy on CAN A: design

Status: **design only, held for the owner.** Nothing here is implemented. It would end
"CAN A is listen-only, always" (CLAUDE.md), so it needs the owner's explicit decision.

Goal (owner, 2026-10-17 and 2026-10-20): read the car's fault codes from an iPhone, live values
second, over the board's direct connection to the car bus. No BT-OBD2 dongle, no PC tool.
Reads only. Clearing codes at most as an explicit extra; never programming, security access or
routines.

## Open questions for the owner

1. **Go or no-go.** Diagnostics mean transmitting on CAN A. That changes the CLAUDE.md rule and
   conflicts with the hardware listen-only mod (security P5). Pick one:
   - (a) diagnostics, plus a P5 variant with a **TX switch** (a physical opt-in, see below);
   - (b) P5 as researched (TX cut for good, no diagnostics ever on this board);
   - (c) neither for now.
2. **Engine running.** Allow reads (`21xx`, DTC read) with the engine running and the car
   stopped? Or only with the ignition on and the engine off?
3. **Clearing codes (`14`).** In the first version, or later? If yes: engine off, speed 0,
   presence window and a confirm.
4. **Opt-in shape.** Armed until the next reboot only (recommended), or a stored setting?
5. **Which ECUs first.** Proposed: injection (S3000), UCH and climate. Then ESP, airbag, BVA,
   UPC?
6. **Sessions.** DDT4all sends `10 C0` before reads and `10 81` after. Do you know which reads
   work without `10 C0`? A request that needs no session change is safer.
7. **DTC text.** The DDT database is Renault's data and cannot go into this public repo. Show
   bare codes plus generic SAE text, or look them up on the phone side from a private file?
8. **Phone side.** The web page first (works today, no app), or a BLE service (needs a native
   iOS app, because Safari has no Web Bluetooth)?
9. **Bench adapter.** Which USB-CAN adapter do you have (CANable/gs_usb, PEAK, a spare ESP32 +
   transceiver)?
10. **Termination.** CAN A still carries the T-2CAN's fixed 120 Ω (RZ2, BACKLOG). Desolder it
    before any transmit on the car bus.

## What would have to change

### The controller (MCP2518FD, `src/bus/Mcp2518Source.*`)

Today `begin()` requests `ListenOnly` and the chip never drives the bus, not even an ACK.

- A request needs **normal mode** (`Normal20B`, classic CAN 2.0B, 500 k).
- The ACAN2517FD library changes mode only through configuration mode, i.e. a new `begin()`.
  That costs a few ms of lost frames each way. Two options:
  - **per transaction:** normal mode for one request and its reply, then back to listen-only.
    The most "listen-only by default". About 2 × (re-init) per request, and the broadcast
    decode has a gap each time.
  - **per diagnostic session:** normal mode while the diag page is in use (armed + a request in
    the last 30 s), listen-only otherwise. Fewer gaps; recommended.
- **Never enter normal mode unless autobaud has locked** (`[can2] bus found: 500k`, clean
  frames). A normal-mode node at the wrong bit rate sends error frames and disrupts the whole
  car bus. Listen-only is safe at a wrong rate; normal mode is not.
- In normal mode the chip ACKs every frame, as every ECU does. That is harmless on a correctly
  terminated bus, and another reason to fix RZ2 first.
- Bus-off or error-passive in normal mode: drop straight back to listen-only, disarm, log
  `[diag]`, and stay disarmed until the owner arms again.

### ISO-TP and KWP2000 (new, `src/diag/`)

- Physical addressing only, one ECU pair at a time. No functional (broadcast) requests.
- ISO-TP: single-frame requests (every allowed request fits in 7 bytes). We send flow control
  (`30 00 00`) only in answer to a first frame from the paired reply id. Reassemble up to about
  64 bytes (DTC replies are 13-40 B).
- KWP2000 replies: positive (`SID + 0x40`), negative `7F SID NRC`. Handle NRC `78` (response
  pending: wait, up to a limit), and give up on `11`, `12`, `22`, `33`.
- `3E` (tester present) only while a non-default session is open, at the ECU's minimum
  interval; `10 81` back to default when done or on timeout.
- Host-testable like AffaDisplay's ISO-TP: a pure state machine over a fake clock and a fake
  link.

### The allowlist: one choke point

Every transmit on CAN A goes through one function that checks, in order:
1. armed;
2. autobaud locked;
3. ECU pair allowed;
4. service and sub-function allowed;
5. car state (see policy below);
6. rate budget.

Anything else is refused, logged, and never reaches the controller.

| ECU (X84, per DDT) | request → reply id |
| --- | --- |
| Injection S3000_A791 | `7E0` → `7E8` |
| UCH_84_J84 | `745` → `765` |
| Climate (Clim_X84) | `744` → `764` |
| later, if asked: ESP `740/760`, airbag `752/772`, BVA `7E1/7E9`, UPC `74D/76D` | |

| Service | Allowed | Note |
| --- | --- | --- |
| `17 FF 00` read DTCs | yes | same on all 9 ECUs (DDT survey 2026-10-20) |
| `18` read DTCs by status | yes | KWP form; only where DDT lists it |
| `21 xx` read data by local id | yes, listed ids only | ids from DDT per ECU, e.g. clim `21C2` |
| `22 xxxx` read data by common id | listed ids only | if an ECU needs it |
| `1A xx` read ECU id | yes | |
| `10 C0` / `10 81` session | only around reads | open question 6 |
| `3E` tester present | only inside a session | |
| `14` clear DTCs | **off**; opt-in only | open question 3 |
| `11`, `27`, `2E`, `3B`, `30`, `31`, `34`-`37`, `10` with other sub-functions, everything else | **never** | the UCH has destructive writes (EEPROM reset, factory mode, key learning) |

Flow-control frames are not services. They are allowed only as the answer to a first frame
from the ECU we just asked.

### Rate limits

- One request in flight on the whole bus. The next one goes ≥ 100 ms after the previous reply.
- At most 5 requests a second in total, and 2 a second per ECU. A diag frame budget of under
  1 % of a 500 k bus (the bus runs about 460 fps of broadcast).
- A DTC read only on demand (a tap), never polled.
- Live values: at most 2 Hz per value, and only while the page is open.
- A timeout (no reply in 1 s, or 5 s after `78`) backs off that ECU for 10 s.

### Policy by car state

The signals already decoded (`SIG_RPM` from `0x181`, `SIG_SPEED` from `0x354`) gate it:

| | ignition on, engine off | engine running, stopped | moving |
| --- | --- | --- | --- |
| DTC read, ECU id | yes | owner's call (Q2) | no |
| `21xx` live values | yes | owner's call (Q2) | no |
| clear DTCs (if enabled) | yes, + presence + confirm | no | no |

When the signals are stale (no fresh rpm or speed within 2 s), treat the car as moving and refuse.

## Listen-only stays the default

Defence in layers, any one of which keeps the bus silent:

1. **Hardware (with P5-switch):** the TX line to the CAN A transceiver goes through a switch or
   jumper on the board, open by default. Open means no image, ours or foreign, can transmit.
2. **Build:** the diag transmit code sits behind `FEATURE_DIAG` (default 0 until it has passed
   the bench). With it at 0, nothing in the image can put the MCP2518FD in normal mode
   (`check_rules.py` gets a rule: `Normal20B` only in `src/diag/`).
3. **Run time:** "Diag requests" is a **menu-only**, volatile setting (like `presenceGate`: no
   `F_WEB`, refused from the form and a file), off at every boot. Arming it also takes the
   presence window (P3).
4. **Every frame:** the allowlist choke point above.

## Conflict with the security work

| Item | Effect on diagnostics |
| --- | --- |
| P2 request gate (`X-MegaOpen`, Host check) | Diag routes are POSTs behind it, like every other change. No conflict. |
| P3 presence | Arming diag needs someone at the board. Clearing DTCs needs it per request. Consistent. |
| P4 signed images | Lowers the risk P5 exists for (a foreign image transmitting). With P4, a switch-based P5 is enough. |
| P5 hardware listen-only | **Mutually exclusive** as researched (option 1 lifts the TXCAN pin for good). The switch variant keeps both: listen-only in hardware by default, TX only when the owner closes the switch. A jumper on the freed UY2 A1 node (10 kΩ to 3V3 when open, joined to TXCAN when closed) fits option 1's rework. |
| CLAUDE.md rule | Becomes "CAN A is listen-only except `src/diag/`, behind the opt-in above". Owner's wording. |

## The iPhone side

| | Web page (`#diag` in the app) | BLE GATT service |
| --- | --- | --- |
| Works on iPhone today | yes (Safari, the board's AP or home WiFi) | **no** without a native app: iOS Safari has no Web Bluetooth |
| Effort | a page + 3-4 routes | a GATT service in BleHub + an iOS app |
| Link | WiFi, already used in the car | the existing BLE bond (AMS/ANCS/HID); a new service adds BLE traffic on a link that has had drops (0x208) |
| Auth | WiFi password + P2/P3 | the bond |

Recommendation: the web page first. Routes:
- `GET /api/diag/ecus` (state, armed, why not);
- `POST /api/diag/dtc?ecu=` (read);
- `POST /api/diag/read?ecu=&id=` (one `21xx`);
- later `POST /api/diag/clear?ecu=` (opt-in).
Results are cached, so a page refresh costs no bus traffic. BLE stays a later option if a native
app ever exists.

## Test plan, before the car

1. **Host tests:**
   - ISO-TP segmentation and reassembly, flow control only after a paired first frame;
   - KWP NRC handling (`78`, `11`, `12`, `22`);
   - the allowlist: every service byte 0x00-0xFF tried against every id, only the listed pairs
     pass;
   - the rate limiter, the car-state policy with stale signals, and the mode state machine
     (normal only when armed and autobaud locked, back to listen-only on bus-off).
2. **Bench, T-2CAN CAN A ↔ USB-CAN adapter** (500 k, 120 Ω at both ends). The adapter runs a
   small ECU simulator (python-can + can-isotp) answering `17FF00` and a few `21xx` with the
   replies from `../MeganeCAN/notes/dt4all log`.
   - **Silent when disarmed:** the adapter logs zero frames from the board across boot, OTA,
     reboot, autobaud and an hour of idle. With only two nodes on the bus, the adapter's own
     frames go unacknowledged while the board is in listen-only: that proves the mode.
   - **Armed:** only allowlisted requests appear. Flow-control timing, rate limits, return to
     listen-only after the session.
   - **Faults:**
     - adapter at 250 k: the board must refuse to arm, and the adapter must see no error
       frames;
     - unplug mid-reply;
     - an ECU answering `78` for 10 s;
     - a reply to an id we did not ask.
3. **Car, stage 1:** ignition on, engine off, the injection ECU only. Read DTCs, compare with the
   DDT4all log of 2026-06-14. The USB-CAN adapter on the OBD port logs bus load and error frames
   in parallel; `[can2]` fps must not change.
4. **Car, stage 2:** UCH and climate. Then live values. Then, only if the owner said yes to Q2,
   with the engine running.

Rough size: `src/diag/` (ISO-TP, KWP, allowlist, policy) about 600 lines plus about 40 host tests;
the Mcp2518Source mode switch about 100 lines; the page and routes about 300 lines.
