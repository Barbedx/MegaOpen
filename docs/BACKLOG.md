# Firmware backlog

The owner's standing ask (2026-09-27): take the firmware to "ideal" over many
sessions, and write down every bug and rough edge as it is seen, so it can be
implemented later. One line of what, one of evidence, one of where. Newest
findings at the top of each section; move an item to **Done** with its commit.

Evidence rule: "seen on camera" means the laptop camera pointed at the bench
panel (`py` + OpenCV, device 0), not inferred from an ACK.

## Findings

The owner's ask (2026-11-03): every inconsistency, bug, suspicion or rough
edge an agent sees goes here first, even when it is off the task, and is not
fixed on the side. The owner asks for a sweep from time to time; then each
entry is fixed (moved to **Done** with its commit), promoted to an Open section,
or dropped with a word why. Newest at the top. One entry: date, what, evidence,
where.

- **2026-11-07 — the USB serial is write-only: no command input.** The web
  installer (`installer/`, ESP Web Tools) has «Logs & Console» with an input
  line; our CDC port shows the log (`src/main.cpp:196`) but nothing in `src/`
  reads `Serial`. Owner's ask: a small command handler on the CDC — the API's
  verbs (`status`, `health`, `logLevel DBG`, `reboot`, `dl` = reboot into
  download mode), one line in, one line out.
- **2026-11-07 — the web installer cannot put a running board into download
  mode.** t2can's USB is TinyUSB (`ARDUINO_USB_MODE=0`, `platformio.ini:135`),
  so the port the browser opens is the app's (PID 80C3); esptool-js's DTR/RTS
  reset makes it re-enumerate as the ROM's USB-JTAG (PID 1001) and the
  install fails «Failed to initialize». BOOT+RST works, but the T-2CAN's two
  buttons carry no labels (owner fumbled it 4 times on the bench). The
  Arduino core already reboots into download on a 1200-baud open of the CDC
  (`USBCDC.cpp` `_onLineCoding`). Options: a «у режим прошивки» button on the
  installer page (open at 1200, close, then Install on the new port), the
  `dl` command above, or one on «Плата». The installer page now has the
  1200-baud button. Left over: after such an install ESP Web Tools' reset
  boots into download mode again — the 1200-baud reboot sets
  RTC_CNTL_FORCE_DOWNLOAD_BOOT (`usb_persist_restart`), esptool clears it in
  its S3 `hard_reset` (arduino-esp32 #6762) and esptool-js 0.6 (inside ESP Web
  Tools 10.4.0) does not: no `RTC_CNTL_OPTION1` in its S3 target. Only a
  power cycle clears it, so the page says «replug». Fix upstream (esptool-js)
  or flash with esptool-js directly from the page and clear the register.

- **2026-11-07 — an OTA upload over the USB link left the USB addressed, never configured.**
  Bench mo-8428 on 1.9.0 (de349c5), WiFi down (AP only). Over the link from a
  PC (tools/usb_link.py's Link): `GET /ota/start?mode=fr&hash=…` answered
  only after >10 s (the client had given up: `91 timeout` on the channel, the
  late `OK` then arrived as the next request's reply); a second try's
  multipart `POST /ota/upload` (1.8 MB) stopped with the host's USB write
  timing out (2 s, then 30 s); from then on no HELLO was read for 14 h.
  Read over its AP on 2026-11-08: the board was fine (uptime 14 h, heap
  49 kB) and **usblink was alive** (its age a few ms in every `[task]`
  line). `/api/status` `usb`: `mounted:false addressed:true sof:true
  unmounts:2 cut:1 dropped:1 txWaitMax:3001`. So the host reset the bus
  mid-transfer, addressed the device again and never configured it. Windows
  kept listing it OK; COM7 would not open ("a device attached to the system
  is not functioning"); DTR/RTS, a 1200-baud touch and a libusb reset did
  nothing; `pnputil /restart-device` needs admin. `POST /api/reboot` over the
  AP fixed it (mounted again at boot). Also: the next `/ota/upload` over WiFi
  failed "Failed to write chunked data to free space" until that reboot, the
  aborted session's Update still open (`GhOta.cpp:656`). Two items: a
  board-side way out of "addressed, never configured" (re-attach the USB
  after N s of it), and closing an Update no upload follows.
  `src/link/UsbPipe.cpp` (watch), ElegantOTA / `src/net/WebUi.cpp` (gateOta).
- **2026-11-07 — the head unit's hotspot drops to 5 GHz, the board loses it.**
  Owner: the tablet switches its hotspot to 5 GHz by default, and then the app
  does not connect (the S3 is 2.4 GHz only). Nothing on the board says so: the
  station just never joins. The BLE link (feat/ble-link) carries the app and the
  board's internet meanwhile; the board could still name the cause (its scan
  sees no `megayozh` on 2.4 GHz while the app says the head unit has a hotspot).
  `src/net/WifiManager.*`.
- **2026-11-07 — "NO HU TEXT" on the panel reads as "no head unit".** Owner: the
  panel lies, it is not clear whether the head unit is connected, yet it shows
  NO HU TEXT. `HuMediaSource::statusText()` says `HU: NO TEXT YET` when frames
  do arrive (the UART link is fine) but DUDU's own player never sent a title:
  the words mix "link" and "content". Candidates: say the link and the content
  apart (e.g. `HU OK, NO TRACK`), or show the app's media (feat/ble-link) and
  keep the HU status for the health page. `src/media/HuMediaSource.cpp:156`,
  `src/app/UiTask.cpp` (the `!linkUp` branch).
- **2026-11-07 — when the app retries is not visible.** Owner: "не зрозуміло
  коли реконект". The Companion's connection loop retried USB, then WiFi, with
  no word on the screen of what it tried or when it tries next; the BLE work
  adds a "next try in N s" line (android/, feat/ble-link). The board side has
  no such line either: `/api/health` chain `app` now says the link
  (`app_usb`/`app_ble`/`app_none`).
- **2026-11-04 — main.cpp is still the glue for every task.** 2867 lines,
  ~165 file-scope globals, `setup()` about 800 lines (1817-2611), 133 `#if`.
  It holds work that belongs to the task modules: the head-unit UART glue
  (`huDrainSerial`, `onHu*`, `huLastFramesJson`) beside `app/HuTask`; the rule
  actions (`performAction`, `macroFire`) beside `app/CarTask`/`KeysTask`; the
  matrix and ladder GPIO and the keys task's hooks (`keys*`); net_svc's hooks
  (`hk*`, `addressText`, `logPushSnapshot`); crash and boot-reason logging. Each
  section could move next to its task, leaving setup() as the wiring list.
  `src/main.cpp`.
- **2026-11-04 — WebUi.cpp is 2559 lines, 64 routes.** The split files
  (`WebUiMap`, `WebUiRules`, `WebUiKnobs`, `WebUiGhOta`, ...) exist, yet
  settings, health, status, display, logs, keys and wifi routes all stay in
  one file. `src/net/WebUi.cpp`.
- **2026-11-04 — State has two writers' paths by build.** With no car task
  (no vehicle source in the build) `loop()` runs `App::Car::service` and
  `g_sniffer.applyRequests()` itself; with one, the car task does. One job,
  two homes chosen by `#if`. `src/main.cpp` loop(), `src/app/CarTask.*`.
- **2026-11-04 — BLE lives in two places.** `src/bluetooth.cpp` and the
  `apple_*_service`/`current_time_service` files sit in `src/`'s root beside
  `src/ble/`; `bluetooth.cpp` serves "legacy top-level keys" in its status
  JSON. The stack is MeganeCAN's byte for byte and BLE is not a priority, so
  moving it is cosmetic until BLE work resumes.
- **2026-11-04 — "macro" names outlive the macros.** Since 1.7 the key
  macros are rules (gesture rows); the runtime is still `Keys::MacroEngine`,
  `MacroTable`, and main.cpp's `g_macroCfg`, `macroFilter`, `macroFire`.
  Works; the names point at a feature that no longer exists on the page.
- **2026-11-03 — older blob layouts are still read.** Not NVS keys but the
  layout versions inside a blob: `Knobs::Config::fromBlob` reads v3/v4
  (`BLOB_SIZE_V3`, the retired potentiometer fields stay in the layout),
  `Rav4Emitter::Tuning::fromBlob` reads v1 and clears a saved "MegaOpen X.Y.Z"
  (builds up to 1.7.0). Dropping a reader resets that board's knob or
  head-unit tuning settings if its blob is the old layout; whether the car's
  is, is unknown. Also legacy JSON: `bluetooth.cpp` top-level AMS keys and
  BleHub's "legacy" mode names. `src/knobs/KnobConfig.*`,
  `src/proto/canbox/Rav4Emitter.cpp`, `src/bluetooth.cpp`.
- **2026-11-03 — the notification popup's default disagreed with itself.**
  `Settings.h` says `ancsPopup` is off by default (a popup covers
  now-playing), but the schema step v2 turned it on for every fresh board.
  Since the migrations moved (`cfg/Migrations.h`) a fresh board takes the
  documented default, off. Existing boards keep their stored value.
- **2026-11-03 — the radio code's key sequence is unverified.** RollUp per
  digit, Load to the next, Load held to confirm comes from one line of
  MeganeCAN's notes; whether the radio accepts `pressKey(…, Wire)` from a
  passive board beside the real panel is unknown. `src/display/RadioCode.*`;
  check in the car: the extension role, a code, battery off, read `[code]`.
- **2026-11-03 — what the extension role draws over the radio is not
  decided.** In the extension role the ui task renders as always, so our
  screens fight the radio's (the CODE prompt, volume popups). Which screens
  should yield is open. `src/app/UiTask.cpp`, `app/StockRadio`.
- **2026-11-03 — `dispType` "updatelist_menu" is no longer converted.** The
  schema step for it was dropped with the old migrate(): a board that still
  stores it reads as Carminat (the registry's first row). Both known boards
  store `carminat`.

## Open

### Hardware (T-2CAN)

- **CAN A listen-only in hardware (security review P5): REJECTED by the
  owner 2026-10-21.** The CAN A TX path stays wired: transmitting there may be
  wanted later (KWP diag polling, writes). Firmware keeps the MCP2518FD in
  listen-only until a feature needs TX. The research (TD501MCAN has no silent
  pin; lift MCP2518FD pin 1 + 10 kΩ pull-up) is in git history, 341ae94.

- **iPhone diag proxy (KWP on CAN A): v2.0, owner 2026-10-21.** Design on
  origin/held/diag-design (docs/design/diag-proxy.md, 10 owner questions);
  DTCs first, reads only. Not before v2.0.

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
  - what each field that starts off shows on the tablet (coolant 0x7D 09,
    tank % in 0x7D 04 D4-D5, the PRND probe 0x7D 06), one at a time;
  - battery volts on the tablet: protocol 112 has no field for them, so
    another field takes them («Звідки» → batt_v ÷10 on range 0x21, tank %
    0x7D 04 idx 5 or the 0x23 bar). Left: which one the tablet shows
    (docs/car-test-can-signals.md).
- **Track position, duration, play state from the head unit.** Protocol 112
  never sends them (com.syu.ms is a platform-signed system app, patching it
  needs root). Path: a user app with a NotificationListenerService reading
  MediaSession, sending to the board over WiFi.

- **Info list: 8 characters — MEASURED 2026-10-25.** Declared 9 to 16 cells
  (one row and all three) all showed 8 on glass: 8 is the width, not a
  library default. The sweep route `/api/display/inforaw` and `Op::InfoRaw`
  are gone (1.5.2); AffaDisplay 1.4.1 records it.
- **Column widths — MEASURED 2026-10-25** (docs/glass-sweeps.md, results).
  Fullscreen rows hold 22 of anything, W and lowercase included, so no pixel
  budget is needed. The menu's rows hold 20 (big font) under a 26-wide header:
  the panel menu and the media menu layout lay rows on 20 since 1.5.2.
- **Right pane seen blank for a while after a notification** in one capture
  (2026-09-27, ~20 s), fine in the next. Observe before acting; could be the
  carousel on an empty scene.

- **Info layout: scroll arrows and row 1.** The panel draws its own scroll
  arrows beside the pane and the byte that hides them is unknown; whether row 1
  (the clock) is visible beside the pane was never answered. Ten minutes on
  `/api/display/raw` with the camera. Where: `MediaScreen` info layout.
- **Never checked on glass:** the `flow`, `tape` and `typewriter` styles,
  `paneSpeed`, the `tape` and `tunnel` scenes, and the fourteen-scene
  `pane: auto` carousel. Host-tested and drawn in the simulator only.
- **Auto-detect the panel family and the head unit — researched 2026-10-21;
  the passive half DONE (see Done: the panel bus watch, the head-unit
  dialect), the switching half not started.** The owner wants more glass time before the board sends
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

- **(Stale, DevKit era) The left (bare) encoder has never run.** Owner 2026-10-28: both encoders and the rings work in the car on the T-2CAN; kept here only as history. Wired on 32/33/36 with R1 2k on
  GPIO36; watch `/api/knobs/live` while it turns. Where: `knobs/KnobSource`.
- **A refused web key goes unlogged when more than 4 are in flight.** The
  verdicts are matched to their tickets in `KeyVerdicts::SLOTS` = 4 slots; a
  fifth key posted before the first is answered still goes out, but a refusal
  of it is not logged. Fine for the console's one-at-a-time keys; raise SLOTS
  if a web macro ever posts bursts. Where: `display/KeyVerdicts.h`.

### Web UI

- **Physical presence for the dangerous routes (security review P3):
  REJECTED by the owner 2026-10-21** as over-engineering for a board in a car
  with a weak AP. origin/held/presence is not merged; the AP password is
  changed on «Плата» (default `megaopen`), so held/ap-pass is not merged either.
- **Signed OTA images (security review 2026-10-21, P4).** Secure boot v2 or
  app-signature verification with the key a CI secret, so only our releases
  flash. Key management and the factory flash change: a project.

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

- **New UI (owner, 2026-10-16): show the board's IP on the display when WiFi
  connects — WRITTEN 2026-10-21, not yet on glass.** On the home network:
  `IP 192.168.x.y` / `megaopen.local` (the mDNS name, when mDNS is on); on
  the board's own AP: `AP MegaOpen-XXXX` / `192.168.4.1`. A fullscreen for
  6 s through the notification path, polite: it waits out the panel menu,
  a switched-off panel and a glass hold (`App::Ui::queueNotice`). Once per
  connection: a link back within 30 s with the same mode and address says
  nothing; a new address, a mode change or 30 s down does
  (`src/net/IpNotice.*`, `test_ipnotice`; fed from net_svc in `main.cpp`
  `hkWifi`). In the car that means one `AP …` notice per drive; a switch
  for it is open if that turns out to be noise.
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
- **Remote logs to GitHub (owner, 2026-10-25) — WRITTEN 2026-10-25, not yet
  run on a board.** The board is in the car on the head unit's hotspot
  (`megayozh`, mobile internet); the owner reads its log from home. Every
  line the flash log keeps also goes to a 1 MB spool in PSRAM; once a minute
  on STA, with the switch on and a token set, a job PUTs what came since the
  last push as one file through the contents API:
  `Barbedx/megaopen-logs/mo-XXXX/boot-NNNNN/PPPP.log` (`sys/boots`, the part
  number this boot). Offline (the garage) the spool fills and the next push
  sends it, 64 kB a file 3 s apart; a spool that wraps first loses its oldest
  lines and the next file says how many bytes. 401/404 stop it until the
  page saves new settings; the rest back off 1 → 10 min. The token
  (fine-grained, Contents read/write on that one repo) is in NVS `logpush`,
  never sent back. One job at a time with GhOta, on the network worker
  (`src/net/NetJob.*`: both move mbedTLS to PSRAM, a global). Плата → «Логи в GitHub»;
  `src/net/LogPush.*`, routes `/api/logpush` (`WebUiLogPush.cpp`), `[push]`
  lines. `FEATURE_LOG_PUSH` on the T-2CAN envs. Open: a token per board does
  not scale to other people's boards (owner's plan); that needs an ingest
  service that issues per-board keys. The previous boot's flash log is not
  pushed (only what the spool holds), so a crash shows up only up to its
  last pushed minute plus the flash log read locally.
- **AffaDisplay: the menu's widths — DONE in v1.4.0/v1.4.1 (2026-10-25).**
  `menuRowChars` 20, new `menuHeaderChars` 26, `kMenuRowCols`/`kMenuHeaderCols`;
  MegaOpen pins v1.4.1 and static_asserts its own widths against them
  (`src/display/PanelOps.cpp`).
- **BLE pairing vs a phone on the board's AP, on the S3 (2026-10-16).** On the
  T-2CAN the iPhone could not pair while it was joined to `MegaOpen-4580`; off
  that WiFi it connected. Same symptom as the 2026-08-07 DevKit bug, fixed then
  by coex prefer-BT and conn params after auth. Not yet confirmed with a
  log: three of the failed tries overlapped board resets caused by opening COM4.
  Re-test with the log over WiFi (no COM4), read `disconnected reason=`.

### USB link

- **The link stopped taking data once (bench, 1.6.5, 2026-10-28), not reproduced.**
  During the first `tools/usb_link.py cable` run (pings of 4..1024 bytes, 4 in
  flight, right after an HTTP-over-link request that the board counted as a
  `fails`), the board stopped accepting host writes after 13 frames: every
  write timed out, through `clear_halt` and a USB reset, while `usblink` kept
  beating and `/api/status` said `mounted: true, rx: 13`. Only a reboot
  brought it back. Fifteen runs after, burst and edge sizes included, never
  did it again. If it comes back: the `[usb]` lines, `/api/status` `usb`, and
  whether `tud_vendor_n_available` stays 0 (the OUT endpoint not re-armed).

### Build / code health

- **Heap corruption on the bench, boot 190 (1.7.3-rc1, 2026-11-01): cause
  unknown.** `multi_heap_free: head != NULL` on the 32-byte http_parser
  block right after the esp_http_client struct, during the log push's
  cleanup, with a GitHub check queued and rc_smoke running; the heap is not
  in the coredump, so the overwriting bytes are unknown. 1.8.0 removed the
  structure around it (the per-job task stacks churning that hole, the
  String replies on httpd, the Settings Strings) and bounded the parser's
  recursion; the trap is the HEAP_SENTINEL build
  (`PLATFORMIO_BUILD_FLAGS=-DHEAP_SENTINEL=1`, `src/utils/HeapSentinel.h`):
  on a hit it logs the address, the step, the task and the bytes around it
  before the coredump, which the next boot pushes to megaopen-logs. Loop on
  the bench: reboot, let the past log push, run `tools/rc_smoke.sh` across
  it, watch `/api/logs` for `[heap] CORRUPT`. The October httpd corruption
  (HEAP_HUNT) was the same class. Watch the car's `[crash]` lines on 1.8.0.
- **Internal heap, two levers not taken yet** (audit 2026-10-21; the log ring
  and request-only statics already moved to PSRAM). Both need a board soak,
  and the first a phone:
  - *NimBLE host memory in PSRAM*, ~8-12 kB: `-D
    CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=1` (NimBLE-Arduino 2.5.1,
    `esp_port/exp_nimble_mem.c`). Moves msys (12 × 256), ACL from the
    controller (12 × 255), the event buffers (30 × 70 + 8 × 257) and the host
    pools; the controller stays internal. Held back: BLE is the riskiest path
    (the 0x208 drops). Test pairing, AMS/ANCS and key latency; read the
    `[mem]` marks around `ble.begin`.
  - *The malloc threshold*, ~3-10 kB (uncertain):
    `SPIRAM_MALLOC_ALWAYSINTERNAL=4096` keeps every malloc/new of 4 kB or
    less internal (String, std::function, NimBLE-Arduino and PsychicHttp
    objects, ArduinoJson pools). `heap_caps_malloc_extmem_enable(512)` first
    thing in setup() lowers it. Risk: a plain-malloc buffer used by DMA or
    with the cache off; check before trying, then compare `[hb]` idle and
    soak.
  - Fixed by the prebuilt sdkconfig, not ours to change: no
    `EXT_RAM_BSS_ATTR` (BSS in PSRAM is off), no task stacks in PSRAM, WiFi
    static RX buffers, the BT controller (`BLE_MAX_ACT` 6), mbedTLS internal
    allocation, the coredump stack.
- **`AFFA_ENABLE_TASK=0` on purpose.** The loop task owns `poll()` and sleeps
  in `TwaiLink::waitRx()` paced by `affa::WakePacer`, the same wake `AffaTask`
  has. It stays the loop because the same pass
  samples the TWAI status, drains the frame trace, publishes `PanelStatus` and
  beats the task registry the OTA trial watches; `AffaTask` has no per-pass
  hook, and an owner loop on `waitRx()` + `WakePacer` is the supported shape.

## Done

- **Nothing older firmware stored is kept or read** (1.9.0): schema v4 turns
  `funcReg` into `panelRole` and removes it, the car screen's index keys,
  `gr1..5` and 1.6.x's tables in the same step; the 1.6.x import (CarStore,
  Config, DecodeTable/FieldMap blobs, the alert/macro blob readers,
  `sigFromSaved`) is deleted, and `check_rules` has no legacy exception left.
  Migrations leave no old key (`Migrations.h`, CLAUDE.md).
- **Health says when the head unit speaks the VW canbox profile**
  (547753c, v1.2.0): `Canbox::HuDialect` counts Toyota-only and VW-only
  inbound commands (the profiles are disjoint) and the 0xF3 NACKs; health
  `hu` carries `dialect` and `notSupported`, and `hu_dialect_vw` when the
  unit is wired right but set to VW. Switching profiles still needs a second
  emitter.

- **The panel bus watch** (715d886, v1.2.0): `MegaOpen::PanelBusWatch`
  listens (never transmits) for another bus master (0x3AF/0x3DF syncs) and a
  lone cluster (0x3CF `69`, no `61 11`). The panel TX gate opens 3 s after
  the first poll, and only with no foreign sync in the last 3 s (one is
  enough to keep it shut); two syncs within 3 s make a foreign master, and
  the stack goes passive, TX-gated and listen-only until 30 s pass without
  one (fixed after review in the commit after 715d886). Health
  `panel_foreign_master` / `panel_cluster_only`. Replays in test_panelwatch;
  the foreign-master path is still to be seen on a bus with an OEM radio.
  Since 1.5.1 only with FuncsReg on: off (passive), MegaOpen draws over the
  radio and the watch only reports (`panel_beside_radio`), as designed.
  **Since 1.9 the watch decides nothing:** the role (`panelRole`, emulator |
  extension, display/PanelRole.h) is the owner's setting, and the watch only
  reports when the bus disagrees (`panel_foreign_master` in the emulator role,
  `panel_no_radio` in the extension role, one `[panel]` line per change). The
  silence loop, the 3 s listen window and our TX gate are gone.

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
