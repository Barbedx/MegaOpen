# Firmware backlog

The owner's standing ask (2026-09-27): take the firmware to "ideal" over many
sessions, and write down every bug and rough edge as it is seen, so it can be
implemented later. One line of what, one of evidence, one of where. Newest
findings at the top of each section; move an item to **Done** with its commit.

Evidence rule: "seen on camera" means the laptop camera pointed at the bench
panel (`py` + OpenCV, device 0), not inferred from an ACK.

## Open

### Hardware (T-2CAN)

- **Swap the LCD backpack for an Adafruit 5545 (STEMMA QT PCF8574), noted
  2026-10-19.** It is the same PCF8574, with STEMMA QT (= Qwiic) on both ends,
  so a stock cable from QWIIC 2 plugs straight in, and P0-P7 are labelled
  pads. That removes the side-mounted Qwiic adapter and the two wires per
  leg on LCD pins 1 and 2. No firmware change: keep the bits (P0-P2 right
  CLK/DT/SW, P4-P6 left), and the address is found by the 0x20-0x27 probe.
  P3 becomes a usable input (there is no backlight transistor). Kamami
  ID 1181302, 28.91 zł, about 4 weeks to arrive.
- **ADS1015 on QWIIC 2 (ordered 2026-10-15, after the CAN capture).** Car
  voltage and the 0-12 V dash dimmer. Chains with the PCF8574 (0x48, no clash);
  the knobs task owns the bus and reads one channel ~every 50 ms. Each input:
  100k/22k divider (14.4 V -> ~2.6 V, 18 V -> ~3.2 V), 100 nF at the pin, a
  3.3 V clamp; never 12 V on a pin; ADC ground = car ground, not the isolated
  CAN ground; +-4.096 V range. PWM-dimmed lighting (unknown on the Megane)
  wants ~1 uF. Bonus: a falling voltage at ignition off can flush the log and
  capture before power is lost (plan Q7). Skip it if the capture shows the
  voltage and dimmer on the bus: both are decoded now as hypotheses (battery
  `0x625` b3, 0.0625 V; dash backlight `0x645` b2), checked by step 8 of
  docs/car-test-can-signals.md.
- **CAN A runs unterminated: desolder RZ2** (fixed 120 R, channel A). Keep RZ1
  (panel). See hardware/schematic/t2can-wiring.artifact.html.

### Display (Carminat panel)

- **Car data on the head unit: decoded, not yet seen on the tablet
  (2026-10-20, owner; rewritten 2026-10-21).** Fuel litres (`0x5C5` b8,
  `0x715` b5), battery volts (`0x625` b3) and the low-fuel lamp are decoded
  from the X84 broadcast matrix (daa4580); gear, consumption and range are
  derived from `0x551` and speed/rpm (daa4580); coolant `0x7D 09`, fuel in
  `0x7D 04` D4-D5 and the computed trip fields go out behind switches that
  are off (88c6b9e). Left:
  - every row is a hypothesis until docs/car-test-can-signals.md is run;
  - what each experiment shows on the tablet (one at a time);
  - battery volts on the tablet: protocol 112 has no field for them (`0x41`
    decodes into the same codes as `0x7D`). Only a borrowed field (0x21
    range, 7D 04 idx 5, a 0x23 bar) is left, wrong unit label and all; on
    the glass and the live page they are there already.
- **Track position, duration, play state from the head unit.** Protocol 112
  never sends them (com.syu.ms is a platform-signed system app, patching it
  needs root). Path: a user app with a NotificationListenerService reading
  MediaSession, sending to the board over WiFi.

- **Info list truncates to 8 characters** (`kInfoCells`), seen on camera with a
  row box that is visibly wider. Same sweep: declared length 0x0B + offsets.
- **Column widths are guesses elsewhere.** `MediaScreen::COLS_DEFAULT = 26`
  still drives the menu/fullscreen media layouts; the glass measured ~22 wide
  characters (capitals/digits) and ~21 lowercase. Proportional font, so the
  real limit is pixels: measure the widest glyphs (W, M) and set the budget.
- **Right pane seen blank for a while after a notification** in one capture
  (2026-09-27, ~20 s), fine in the next. Observe before acting; could be the
  carousel on an empty scene.

- **Info layout: scroll arrows and row 1.** The panel draws its own scroll
  arrows beside the pane and the byte that hides them is unknown; whether row 1
  (the clock) is visible beside the pane was never answered. Ten minutes on
  `/api/display/raw` with the camera. Where: `MediaScreen` info layout.
- **Never checked on glass:** the `flow` style, `paneSpeed`, and the
  twelve-scene `pane: auto` carousel. Host-tested only.
- **Auto-detect the panel family and the head unit — researched 2026-10-21,
  NOT started.** The owner wants more glass time before the board sends
  anything it is not sure of. Findings:
  - `displayType` (carminat | updatelist) is already a runtime NVS setting
    (Settings.h, SettingsRegistry, AffaPort.cpp builds the object, reboot to
    apply); both families are compiled into t2can.
  - A passive (listen-only) check cannot tell Carminat from UpdateList: before
    registration the panel sends the same `0x3CF` ping and `61 11 01` either
    way, and the bench panel is universal (it opened both `1C1 70` and
    `0A9 70`; archive/bench-notes `docs/BENCH-VERIFIED.md`). The family is set
    by who opens the handshake (`0x3AF` vs `0x3DF`), not by the panel.
  - Passively detectable: a cluster (only `3CF` DLC 1 `69`, never `61 11`),
    and a foreign master (an OEM radio's `0x3AF`/`0x3DF`) on the panel bus.
  - Proposed `displayType=auto`: listen ~1 s; foreign master → stay silent and
    say so in health; cluster → say so; else try Carminat, and if no `1C1 70` /
    registration ACK in ~250 ms rebuild as UpdateList (the display object is
    heap-allocated in AffaPort, cheap). Remember the winner in NVS like
    `can2Detected`. App-side only, no library change. For this universal panel
    it almost always ends at Carminat.
  - Head unit: only the RAV4/Toyota emitter exists. The incoming command set
    names the profile (Toyota `0x81/0x83/0x84/0x90/0x9D`, VW `0xC0..0xC4`,
    `0xF3` = not supported, raise-protocol.md), so a health warning "head unit
    speaks VW, firmware speaks Toyota" is cheap; switching needs a second
    emitter first.

### Keys / input

- **The left (bare) encoder has never run.** Wired on 32/33/36 with R1 2k on
  GPIO36; watch `/api/knobs/live` while it turns. Where: `knobs/KnobSource`.
- **SK6812 RGBW strips are not supported** (WS2812B GRB / RGB only).
  Where: `knobs/LedStrip`.
- **A refused web key goes unlogged when more than 4 are in flight.** The
  verdicts are matched to their tickets in `KeyVerdicts::SLOTS` = 4 slots; a
  fifth key posted before the first is answered still goes out, but a refusal
  of it is not logged. Fine for the console's one-at-a-time keys; raise SLOTS
  if a web macro ever posts bursts. Where: `display/KeyVerdicts.h`.

### Web UI

- **`/api/knobs` stays out of the settings registry (decided 2026-10-21).**
  Slice 7 folded `/api/cap/settings`, `/api/loglevel`, `/api/routing`,
  `/api/messages/enable` and `/api/canbox/tuning` into `/api/settings` (9
  routes off the 112-route table). `/api/knobs` was left on purpose: its GET
  carries the board's pins, pull-ups and which knobs are fitted, and its POST
  the key bindings (Keys::Binding per turn/click/hold) and eight colours per
  LED zone, none of which is a registry shape (Bool/Int/Enum/Table/Str); the
  scalar parts (lights on, brightness, react/idle/colour preset per knob) are
  already registry entries. Folding the rest would free no route and would
  need a binding type and a colour type first. Revisit only if the knob page
  is redesigned.

- **Settings export/import (/api/config) — UI card removed in ui-v2 slice 8
  until built.** The Плата screen had "Налаштування у файл" calling
  `GET`/`POST /api/config`, which the firmware never had (404). The plan lists
  it as the one route that replaces nothing. Where: `src/net/WebUi.cpp`,
  `web/js/80-board.js`, `web/sim/mock.js`.
- **New UI (owner, 2026-10-16): show the board's IP on the display when WiFi
  connects.** `megaopen.local` answers now (see Done), but Android before 12
  has no `.local`, so the address can still be unknown without a router or
  a LAN scan. A popup/splash with `IP 192.168.x.y` for a few seconds on STA
  connect; in AP mode `192.168.4.1`. Part of the UI rework (mockup:
  ui-audit workflow 2026-10-16).
- **Firmware updates over the internet from GitHub Releases (owner,
  2026-10-19) — WRITTEN 2026-10-21, not yet run on a board.** Плата → «З
  GitHub»: Перевірити reads `releases/latest` (public repo, no token; finals
  only, prereleases are skipped), compares the tag with `MEGAOPEN_VERSION`
  (an rc orders below its final); Оновити downloads
  `megaopen-<env>-<ver>.bin` into the free slot while hashing it, and sets the
  boot partition only when the SHA-256 matches the release's `SHA256SUMS`;
  then it WAITS: the reboot into the usual OTA trial comes from the page's
  «Перезавантажити», or by itself once the speed is fresh and 0 (a power-off
  boots it too). Never automatic; install and reboot refused in AP mode and
  while the car's speed is fresh and above 3 km/h. One OTA owner: the local
  upload (`/ota/*`) answers 409 while a GitHub job runs or waits, and an
  install clears a local session idle for 60 s. Every URL, first and after
  each redirect, must be https on a GitHub host (`GhRelease::trustedUrl`). `src/net/GhOta.*` (task, TLS),
  `GhRelease.*` (decisions, `test_ghota`), `GhRoots.h` (the trusted roots,
  chains checked 2026-10-21), routes `/api/fw*`. `FEATURE_GH_OTA` is on for
  the T-2CAN envs, off for `direct` (no PSRAM for the handshake). Still to
  measure on the board: internal heap during a download (`[ota] github: …
  heap min`, also in `/api/fw`). Known limit: the API allows 60 unauthenticated
  checks an hour per public address, and the page says so on a 403.
- **BLE pairing vs a phone on the board's AP, on the S3 (2026-10-16).** On the
  T-2CAN the iPhone could not pair while it was joined to `MegaOpen-4580`; off
  that WiFi it connected. Same symptom as the 2026-08-07 DevKit bug, fixed then
  by coex prefer-BT and conn params after auth. Not yet confirmed with a
  log: three of the failed tries overlapped board resets caused by opening COM4.
  Re-test with the log over WiFi (no COM4), read `disconnected reason=`.

### Build / code health
- **`AFFA_ENABLE_TASK=0` on purpose.** The loop task owns `poll()` and sleeps
  in `TwaiLink::waitRx()` paced by `affa::WakePacer`, the same wake `AffaTask`
  has. It stays the loop because the same pass
  samples the TWAI status, drains the frame trace, publishes `PanelStatus` and
  beats the task registry the OTA trial watches; `AffaTask` has no per-pass
  hook, and an owner loop on `waitRx()` + `WakePacer` is the supported shape.
- **AffaDisplay docs disagree with its code in ~12 places** (API.md §2.10
  `Esp32CanLink`, §4.1 RX topology, TX queue depth 6 not 4, payload 119 not
  113, the missing `examples/19_owned_task`...). Ticket upstream, with the L1
  debug callback guard.

## Done

- **A menu knob commit is no longer lost under a web save** (review,
  2026-10-21): closed by the settings lock (3ad4450). The /api/settings
  handler and every other web route hold it for the whole request, and the
  menu's applyPending() for each commit, so the POST's config() ...
  setConfig() and a menu commit cannot interleave. Written down in
  `knobs/KnobService.h`.

- **A menu Bool pressed twice toggles twice** (review, 2026-10-21): the
  engine writes each commit into the menu's own copy at once, and the copy
  is re-read only once the commit queue is through (refusals included).

- **No automatic screen over a live popup** (2026-10-21): every now-playing
  layout, the car screen and the track splash wait for the popup's deadline
  (`display/PopupGate.h`, one gate in `UiTask.cpp`) and go on the first pass
  after it; a volume popup now lives its full `keyPopupMs`. Not seen on glass
  yet.

- **DDT-derived names kept in the public repo, owner's decision 2026-10-21.**
  `docs/signal-glossary.*` and `tools/can_corr.py` keep the Renault DDT ECU
  file names, field encodings and French parameter names; nothing stripped.

- **A DNS name for the board** (owner, 2026-10-19): `megaopen.local`
  resolves at home, checked 2026-10-21 (`cfg` `mdns`/`mdnsOn`, default
  `megaopen`). A fixed DHCP lease stays the fallback for phones without
  `.local`.
- **Car CAN from the X84 broadcast matrix** (2026-10-20/21): 39 rows (fuel,
  battery, pedals, belts, roof, climate, dynamics...) with Renault's
  positions, gear/consumption/range derived, verified/age per signal
  (daa4580); the live page `#live` and panel alert rules (52ea045, alerts in
  `/api/settings` since be88f5b); head-unit experiments (88c6b9e). Review
  fixes: steering centred on 0x7FFF, the 2-bit states' invalid values no
  longer read as yes (`DF_STATE2`), belts and seat read 1 = yes
  (`DF_STATE2_INV`). Car checks: docs/car-test-can-signals.md.

- **OTA trial is a health check, not the display** (owner's call): any OTA
  image is confirmed after 60 s up with WiFi seen once and >20 kB heap; a crash
  before that, or 180 s unhealthy, rolls back. Verified on the board: trial
  true at 20 s, confirmed by 68 s.

- **Head-unit text goes through the mailbox** (was a direct setText): counted,
  and held against the media repaint like any other posted screen.

- **`sendFullscreen` takes a `CarminatDisplay&`**; other panels fall back to
  the library's fullscreen. ResistiveMap initialisers match `loadDefaults()`;
  `hybrid_batt` capped at 8 bars; bus-off log and probe comment corrected.

- **CAN pad routes** (`/api/can/pintest`, `/pads`) test the NVS pins the bus
  runs on, and say which. `/api/can/listen` and `/timing` keep refusing in
  the display role (it would drop the panel session) but no longer blame the
  library for it.

- **Rav4Emitter on-change** hashes the whole payload (was the first 4 bytes):
  byte 4 of 0x1E/0x28/0x31/0x58 now goes out at once. Test added, failed
  before the fix.
- **Board V reports real bus errors** (`twai_status_info.bus_error_count`).

- **`?` on glass**: typographic apostrophes (U+2019/U+02BC — every "п'ятий"),
  quotes, dashes and the ellipsis are mapped, emoji dropped, before the
  library's toAscii (`display/TextFold.h`, host-tested). Titles and
  notifications both. Seen on camera: "Vi: p'yatij test ok".
- **Long notifications** wrap at word boundaries and end in `..`.
- **Test hooks**: `/api/display/notify?title=&body=` (the phone's path),
  `/api/display/clear` (back to the media screen), and a web popup now closes
  itself after 3 s (`ms=0` keeps it). All seen on camera.

- **Macro windows now expire on devkit/c3**: `g_macros.tick()` runs from
  `loop()` when there is no scan task. Builds; not exercised live (no macro is
  enabled on the bench board).

- **setText's 8 characters are the panel's limit**, not a length byte: declared
  lengths 0x0E..0x20 all showed "ABCDEFGH" on glass. Long text needs the
  marquee or a fullscreen.
- **Next-track latency, measured**: iPhone ~1.0-1.3 s from the AMS command to
  the new title; firmware paints it ~65 ms later. What looked like 3-4 s was
  the camera (~1.5 s lag) plus the marquee opening mid-string.
  Fixed the marquee (restarts at the first character, holds 1.2 s) and cut
  SPLASH_SETTLE_MS 700 -> 300.

- **No key popup for track keys** (SrcNext/SrcPrev and the wheel) while the
  track splash is on: the splash is the feedback (owner's call). Volume keys
  keep their popup. Seen on camera: next track, no popup, splash at ~4 s.

- **Notifications are a fullscreen, not a one-row popup** — sender on row 1,
  body word-wrapped over rows 2-3, 6 s, then the media screen comes back.
  Seen on camera with two real notifications back to back.
- **Popup icons**: the default popup icon byte 0x09 lit i(TRAFFIC), AF-RDS and
  LIST on glass; 0x55 lights none (seen on camera). Default changed.
- **The Text-layout marquee wrote into an open popup's frame** (seen on camera):
  the Text layout no longer repaints while a popup is up.
- **Track splash** uses the OEM fullscreen geometry: three full rows.
