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
  voltage and dimmer on the bus.
- **CAN A runs unterminated: desolder RZ2** (fixed 120 R, channel A). Keep RZ1
  (panel). See hardware/schematic/t2can-wiring.artifact.html.

### Display (Carminat panel)

- **Car data the head unit cannot get yet (2026-10-20, owner).**
  - *Fuel level and battery volts:* no broadcast id known; diag 743 `2112`
    returns both (KWP polling, see the diag memory). Or hunt the bus: log
    listen-only across a refuel and diff the slow bytes (0x5C5 byte 0 is
    partly unexplained). The RAV4 profile shows no battery volts anywhere;
    the owner wants them visible even in a borrowed field (0x21 range, 7D 04
    index 5, or a 0x23 bar), wrong unit label and all.
  - *Coolant 7D 09:* the signal is decoded (0x551 b0 -40), only the
    Rav4Emitter builder is missing.
  - *Trip data* (avg speed, consumption, range): computed by the cluster,
    not broadcast; derive from 0x551 and 0x5FD (signal-glossary.md 87-92).
- **Track position, duration, play state from the head unit.** Protocol 112
  never sends them (com.syu.ms is a platform-signed system app, patching it
  needs root). Path: a user app with a NotificationListenerService reading
  MediaSession, sending to the board over WiFi.

- **Info list truncates to 8 characters** (`kInfoCells`), seen on camera with a
  row box that is visibly wider. Same sweep: declared length 0x0B + offsets.
- **Fix the fullscreen in the library, then delete ours.** AffaDisplay v1.0.0
  `showFullscreenText` declares 0x60 with byte 5 = 0x40 and prefixes two spaces:
  row 1 is indented and row 3 is starved. `MegaOpen::sendFullscreen`
  (PanelOps.cpp) uses the OEM 0x69 / 0x49 / text at 32 and replaces it for
  notifications and the track splash. Upstream it as a new tag
  (refresh the cache, do not retag v1.0.0).
- **Column widths are guesses elsewhere.** `MediaScreen::COLS_DEFAULT = 26`
  still drives the menu/fullscreen media layouts; the glass measured ~22 wide
  characters (capitals/digits) and ~21 lowercase. Proportional font, so the
  real limit is pixels: measure the widest glyphs (W, M) and set the budget.
- **Right pane seen blank for a while after a notification** in one capture
  (2026-09-27, ~20 s), fine in the next. Observe before acting; could be the
  carousel on an empty scene.

- **A popup is still painted over in the non-Text layouts.** Only the Text
  layout stops repainting under a popup; the others are slowed to
  `POPUP_PAINT_MS` (1 s) and the next screen covers the popup on this panel,
  so a volume popup lives ~1 s. Where: `main.cpp` media repaint (`underPopup`).
- **Info layout: scroll arrows and row 1.** The panel draws its own scroll
  arrows beside the pane and the byte that hides them is unknown; whether row 1
  (the clock) is visible beside the pane was never answered. Ten minutes on
  `/api/display/raw` with the camera. Where: `MediaScreen` info layout.
- **Never checked on glass:** the `flow` style, `paneSpeed`, and the
  twelve-scene `pane: auto` carousel. Host-tested only.

### Keys / input

- **Default routing gives a phone-only user dead volume keys.** `routeHid =
  VOLUME` but `bleMode` defaults to `ams` (HID off), and `routeUart = ALL`
  assumes a head unit. A "phone only" preset exists on the index page; the
  default is a design call. Where: `cfg/Config.h` (`routeAms/Hid/Uart`, `bleMode`).
- **`AmsKeySink` accepts classes it does nothing with.** Default `routeAms`
  includes `CLASS_PHONE`; `Phone`/`Voice` (and `Mute`) fall into `default:
  break`. The mask promises what the sink does not deliver.
  Where: `sinks/BleKeySinks.cpp` `amsHasCommand` (the AMS half of
  `BleKeyWorker`; the mask is applied in `sinks/BleKeyProxy.cpp`).
- **The left (bare) encoder has never run.** Wired on 32/33/36 with R1 2k on
  GPIO36; watch `/api/knobs/live` while it turns. Where: `knobs/KnobSource`.
- **SK6812 RGBW strips are not supported** (WS2812B GRB / RGB only).
  Where: `knobs/LedStrip`.
- **A refused web key goes unlogged when more than 4 are in flight.** The
  verdicts are matched to their tickets in `KeyVerdicts::SLOTS` = 4 slots; a
  fifth key posted before the first is answered still goes out, but a refusal
  of it is not logged. Fine for the console's one-at-a-time keys; raise SLOTS
  if a web macro ever posts bursts. Where: `display/KeyVerdicts.h`.

### BLE

- **An AMS volume step is 15 writes-with-response, 450-750 ms of radio time.**
  Read `canWriteNoResponse()` on Remote Command and use write-without-response
  if offered (log which path); otherwise change the burst's shape. HID is
  already one notify. Where: `sinks/BleKeySinks.cpp:48`,
  `apple_media_service.cpp` (`writeValue(..., true)`).

- **Long track titles are cut at the AMS entity-update size.** The entity
  attribute characteristic (for the rest of a long value) is not used, and its
  existence check looks up `APPLE_REMOTE_COMMAND_UUID` by mistake.
  Where: `apple_media_service.cpp` ~line 95.
- **ANCS control point can be freed under a write.** `requestAttributes()`
  now reads `gControlPoint` once, but a reconnect's `getClient()` ->
  `deleteServices()` can still free it between that read and `writeValue()`.
  The ANCS lock cannot be held across the write (the host task takes it in the
  notify callback while the write waits on the host). Needs a use-count or a
  loop-task-owned detach. Where: `apple_notification_service.cpp`.
### Web UI

- **Fold the per-feature settings routes into the registry (slice 7),
  deferred 2026-10-20.** `/api/settings` is registry-driven since 718c354, but
  `/api/knobs` (settings half),
  `/api/canbox/tuning` still
  keep their own handlers; folding the safe ones would take 4-6 routes off the
  112-route table (WebUi.h ROUTE_MAX). One route per commit, each page moved to
  `/api/settings` in the same commit.

- **Settings export/import (/api/config) — UI card removed in ui-v2 slice 8
  until built.** The Плата screen had "Налаштування у файл" calling
  `GET`/`POST /api/config`, which the firmware never had (404). The plan lists
  it as the one route that replaces nothing. Where: `src/net/WebUi.cpp`,
  `web/js/80-board.js`, `web/sim/mock.js`.
- **The direct tier has no web screens for the matrix keymap or the KEY1/KEY2
  ladder.** The old `/res` page went with the ui-v2 cutover; only the API is
  left: `/api/keymap*` (FEATURE_MATRIX) and `/api/res`, `/api/res/live`,
  `/api/res/map`, `/api/res/auto`, `/api/res/learn` (FEATURE_RESISTIVE_KEYS),
  both on only in `direct`. The owner plans an OLED for that tier.
  Where: `web/js/60-keys.js`, `src/net/WebUiRes.cpp`, `WebUi.cpp` keymap.
- **New UI (owner, 2026-10-16): show the board's IP on the display when WiFi
  connects.** mDNS fails on the T-2CAN, so after joining a home network the
  address is unknown without a router or a LAN scan (first time: found
  by ARP). A popup/splash with `IP 192.168.x.y` for a few
  seconds on STA connect; in AP mode `192.168.4.1`. Part of the UI rework
  (mockup: ui-audit workflow 2026-10-16).
- **A DNS name for the board (owner, 2026-10-19).** Reach it as
  `megaopen.local` instead of an IP found by ARP. NVS already has
  `mdns`/`mdnsOn` (default `megaopen`, `cfg/Config.cpp`), but mDNS does not
  answer on the T-2CAN (item above). First find out why: trimmed in the heap
  work, not started in STA, or blocked by the network. If mDNS cannot work on
  some phones (Android before 12 has no `.local`), a fixed DHCP lease on the
  router is the fallback; say so on the Плата screen.
- **Firmware updates over the internet from GitHub Releases (owner,
  2026-10-19).** A tag builds `firmware-t2can.bin` (and the other envs) in
  GitHub Actions and attaches it to a Release with its SHA-256. The board, in
  STA mode, reads `releases/latest` from the GitHub API, compares the tag
  with `fw`, and on the owner's tap on Плата downloads the asset over HTTPS
  into the free OTA slot (esp_https_ota), checks the hash, and reboots
  through the same trial/rollback as the local OTA. Questions to settle
  first: whether the repo is public (a private one needs a token on every
  board); whether it is automatic or only "check + button" (never mid-drive:
  only with ACC on and the car parked, or from the page); and whether the TLS
  heap fits (~40 kB; on the T-2CAN put mbedTLS in PSRAM).
- **BLE pairing vs a phone on the board's AP, on the S3 (2026-10-16).** On the
  T-2CAN the iPhone could not pair while it was joined to `MegaOpen-4580`; off
  that WiFi it connected. Same symptom as the 2026-08-07 DevKit bug, fixed then
  by coex prefer-BT and conn params after auth. Not yet confirmed with a
  log: three of the failed tries overlapped board resets caused by opening COM4.
  Re-test with the log over WiFi (no COM4), read `disconnected reason=`.

- **`uartBaud` has no write path.** Loaded/saved under NVS `baud` (38400),
  reported by `/api/settings`, but no POST sets it.
  Where: `cfg/Config.cpp`, `WebUi.cpp` settings POST.

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
- **Renault DDT data in the repo: a licence question before it goes public.**
  `docs/signal-glossary.*` and `tools/can_corr.py` carry data derived from the
  Renault DDT database (ECU file names, field encodings, French parameter
  names). Settle whether that may be published, or strip it, first.

## Done

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
