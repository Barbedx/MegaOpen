# Stabilisation 1.12 — plan

Status: REVISION 3, 2026-11-22 (rev 1 and rev 2 reviewed: GO WITH CHANGES;
this text takes every change). Base: 1.11.0 (38249bd) + 1a542d8 on `fix/usb-event-stack`.
Target: `v1.12.0-rc1`, bench OTA-verified, then the car.
The build's sdkconfig: `framework-arduinoespressif32@3.20017…/tools/sdk/esp32s3/qio_opi/include/sdkconfig.h`
(`platformio.ini:42,94`), not the 3.x `-libs` package.

## Why

Car mo-4580, 2026-11-22, boots 282–292 (megaopen-logs), all on 1.11.0:

| # | what | evidence |
| --- | --- | --- |
| E1 | panic ×2, stack overflow in core task `arduino_usb_eve` (2 kB) | coredumps 289/290: `Log::write` ← `UsbPipe.cpp:98` onEvent lambda, `exccause 0x41`, stack 1056/980 |
| E2 | app over USB never gets HELLO_ACK, retries every 5 s | boot 291 25–94 s, `via usb`→`none` 3 s later; ACK = 8+118+2 = 128 B with a 5-digit `upMs`; 129/130 B worked (456/1690 s). No public vendor ZLP call (`tud_vendor_tx_cb` and `usbd_edpt_xfer` exist); `VENDOR_TX_BUFSIZE 64`; app reads 16 KB / 250 ms (`UsbLink.kt:137`) |
| E3 | internal heap dipped to 4.7 kB, cause unknown | boot 292 `[win] min=4752 big=1524` for the window 62.9–72.9 s. TLS in NetJob already allocates in PSRAM (`NetJob.cpp:39-67`); the failed TLS push had ended at 39.7 s, so it is not the cause. `big` sits at ~10 kB for most of the boot |
| E4 | ~20–25 ms both-core stalls every ~2.4 s | `[stall] loopGapMax` 19.5–24.6 ms, `pollLateMax` 24.9 ms, AFFA CompleteCb overruns (boots 286–288), `[can2] hwOvf` ≤2; capture writes a 16 kB chunk every ~2.4 s (`CanRecorder.cpp:29`, ~6.6 kB/s); W/E lines flush the flash log (`LogFile.cpp:152-153`) |
| E5 | `applink` blocks for seconds | `applink busy=3597703us` in a 10 s window; push-through-app failures at 597/673 s; loopback `connect/send` 5 s, TX wait 3 s (`LinkSession.cpp:84-109`, `UsbPipe.cpp:62-83`) |
| E6 | LOG in foreign callbacks besides USB (preventive) | WiFi event handler `WifiManager.cpp:42-48`; NimBLE callbacks log with `%s` (`BleHub.cpp:250-339`, `BlePipe.cpp:50-57`). No failure seen there yet |
| E7 | no task watchdog of ours | no `esp_task_wdt_add` in `src/`; only the core's IDLE0 WDT (5 s, panic) |

Owner's decisions (2026-11-22): watchdog logs only in 1.12; net jobs get a
heap budget; capture: "the board is online all the time, it sends fast; make
it more compact, send the changes" (supersedes "ring in PSRAM only").

## Rules this release introduces

R1. Foreign contexts (esp_event/WiFi/USB event handlers, `tud_*_cb`, ISRs)
only count, flag, post or notify; `LOGQ` is their log. NimBLE callbacks keep
their behaviour this release (BLE = MeganeCAN's code; "invest nothing") and
are measured first.
R2. No pass of `applink` blocks longer than ~20 ms.
R3. Big network jobs ask a measured heap budget; user-initiated ones are never
held silently.
R4. Bulk flash writes are paced so that none stalls the cores > ~8 ms.
R5. `tools/check_rules.py` enforces R1 (scanner) and long timeouts in
`src/link`; its scaffolding lands once, in S2.

## Sequencing

Parallel only where files do not meet:
- **Lane A (link):** S1 → S5 (both in `UsbPipe::write`, `LinkSession`).
- **Lane B (rules + logging):** S2 → S6 → S7 (`check_rules.py`,
  `Housekeeping.cpp`, `Log*`).
- **Lane C (net):** S3 (`NetJob`, `LogPush`, `GhOta`, the failed-alloc hook);
  its `[win]` context lives in lane B (S6), not in `Housekeeping.cpp` here.
- **Lane D (capture):** S4 after S3 and S5 have merged (budget; big bodies
  through the app).
Merge order: S1, S2, S3, S5, S6, S4, S7. Status JSON / `web/i18n` additions:
each slice appends its own keys; conflicts are resolved at merge.

## Slices

Each is one or a few commits; builds `t2can`, `t2can_usbhid`, `direct`;
`pio test -e native`; `check_rules`, `check_pages` when web/ changes.

### S0 — done (E1)
1a542d8: onEvent counts only, `watch()` logs on applink; `W [link] HELLO_ACK
not sent`; `[link]`/`[usbbus]` status lines.

### S1 — no USB transfer ends on a 64-byte boundary (E2)
- In `UsbPipe::write`: before a frame, wait (bounded, same give-up) until the
  FIFO is empty (`tud_vendor_n_write_available() == 64`, i.e. the previous
  transfer ended); then, when the frame length `n % 64 == 0`, append one
  filler byte `0x00` **in the same write**. That makes every transfer end on a
  short packet regardless of coalescing. (Unverified: that the prebuilt
  vendor driver re-arms on IN completion; the bench shows it.)
- The filler only travels board→host: `tools/usb_link.py:167,172` ignores a
  lone `0x00` between frames (not `bad`); the APK counts it as `bad` until a
  later APK.
- Host test: frames of 64/128/192 B get the filler; HELLO_ACK for every
  `upMs` width; others untouched.
- Verify: **only with the Android APK** (PC libusb returns partial data on a
  timeout and hides the bug): bench tablet or the car — `app via usb` holds
  after a HELLO at 10–100 s uptime; `[link] … ackFail=0`.

### S2 — `LOGQ` and the non-BLE foreign handlers (E6, R1, R5)
- `LOGQ(cat, fmt, a0..a3)`: 28-byte POD (literal `cat`/`fmt` pointers, 4 ×
  uint32, ms) in a 64-slot ring under a `portMUX` (ISR-safe), drained and
  formatted by the logbus task; numeric args only; `drops` in `[hb]`.
- Convert: the WiFi event handler (`WifiManager.cpp:42-48`), any esp_event /
  `tud_*_cb` / ISR log the scanner finds. NimBLE callbacks untouched.
- `check_rules.py`: scaffolding for multi-line scans + the R1 scanner
  (lambdas passed to `.onEvent(`/`WiFi.onEvent(`, `tud_*_cb`, `IRAM_ATTR`
  bodies; NimBLE callback classes reported as a warning list, not an error);
  escape `// ctx: task <name>`.
- Verify: `LOGQ drops=0` in `[hb]`; no `exccause 0x41`.

### S3 — measure the heap dip, then a budget (E3, R3)
- **Measure first** (the `[win]` sampler is lane B's, S6): at each new window
  minimum record `at=<ms>` and context flags — `NetJob::running()`, an httpd
  request open, BLE connections, the WiFi state — as `[win] min=… big=…
  at=<ms> ctx=job,http,ble1,join`; register
  `heap_caps_register_failed_alloc_callback` that only counts and keeps the
  last size/caps/task (logged later by `LOGQ`/the heartbeat, R1).
- **Budget:** `NetBudget::admit(path)` — thresholds constexpr, set from the
  measured need of the WiFi TLS path (record buffers are 16 kB each in PSRAM
  via `NetJob.cpp:39-67`; what stays internal is the lwIP/WiFi side) with no
  "largest block" gate above that need (`big≈10 kB` is normal); lower for the
  app path. Background jobs (log/status/CAN/coredump push, the periodic
  release check) are held when refused: retried next tick, no backoff growth,
  `I [push] held: heap free=… big=… need=…` once per episode, a `held`
  counter. User-initiated (`/api/fw/install`, «Перевірити») never held: they
  run, or answer `e.heap_low` (i18n uk/en).
- Verify: the `[win] … task=` line names the dip's owner on the car; pushes
  `held` around dips instead of failing; no `logpush`/`httpd` panics.

### S4 — capture: compact, sent from RAM, flash paced (E4, R4)
Depends on S3 (budget) and S5 (big bodies through the app). Lands last but S7.
- **Format v2** (`CapFormat`, new magic `MOCAP2`; header byte 15 = 0 meaning
  variable-length records). Each **segment is self-contained**: a header, the
  full id table, an absolute time, and a full-data keyframe per id before its
  first delta — so a pushed, spilled or lost segment never breaks another, and
  a segment cut short decodes to its last whole record (v1's promise,
  `CapFormat.h:11-13`).
- Records: a kind/flags byte, a varint time delta, an id index (1 B, escape
  `0xFF` + the full id word with bus/ext bits for ids past 254 or new in the
  segment, which also enters them in the table), then per kind: FRAME/KEYFRAME
  = changed-byte mask + changed bytes (len change = full data); STATS = index
  + count varint; MARKER, GAP, BITRATE, MODE = their v1 fields, varint-packed.
  Target ≤ 6 B average for FRAME; STATS (~44/s for 44 ids) counted in the
  measured average.
- Decoders read v1 and v2: `tools/can_dump.py` (today 18-byte only,
  `:38-49,68-69`), `tools/cap_fetch.py`, `web/js/70-car.js`; tests in
  `test/test_capture` (round trip, segment independence, truncation, id
  escape, all kinds) and a replay of a real car v1 file → v2 with the size
  ratio printed.
- **RAM ring:** log `[mem] psram free=… big=…` at boot (after the capture
  queue 1 MB and the push spool 1 MB); size the encoded ring from it, keeping
  ≥ 1.5 MB PSRAM free for transients (PUT bodies, TLS in PSRAM, WiFi/lwIP);
  default request `cap.ramMB` = 2 (new key; `cap.ring` stays the Bool it is).
- **Segments:** sealed at 64 kB or 60 s, whichever first. Persistent sequence
  from the recorder's `next` (`CanRecorder.cpp:141`); path
  `cap/SSSS/rNNNN.bin` for RAM-pushed segments so it never collides with
  spilled `NNNN-PP.bin` files; 422 = already there, as today.
- **Send from RAM** while online (WiFi or the app) and admitted by S3 and the
  day budget (`s_cused`, unchanged): push sealed segments, free them on 2xx /
  422. Progress kept in RAM; NVS written only on spill or when standing (no
  NVS per PUT).
- **When it cannot send** (offline, S3 hold, budget spent): the ring fills;
  when it would overwrite an unsent segment, spill to flash in 4 kB pieces
  with `vTaskDelay(2)` between — **if the bench shows a gap > 8 ms per piece
  (erase), spill only when standing** (`SIG_SPEED` 0 and `SIG_HANDBRAKE`,
  `Signals.h:51,65`) and otherwise overwrite the oldest segment, leaving a GAP
  record. The web «зберегти» spills now, refused while moving (`e.moving`).
- **Loss bound:** a crash or power cut loses the unsealed tail (≤ 60 s) plus
  unsent sealed segments still in RAM (≤ the ring); stated in the status
  (`unsent n kB`).
- **Log flush:** W/E lines flush at most every 5 s (size trigger and the
  reboot/panic flush kept).
- Odo/service NVS stays where it is (already ≤ 1/min, on change).
- Verify: `[cap] … B/rec` average; `[push] can r…` segments while driving;
  `[stall] loopGapMax` < 8 ms while driving online; offline: the spill policy
  line and its measured gap.

### S5 — applink never blocks (E5, R2)
- Loopback sockets non-blocking (`connect` EINPROGRESS polled; `send
  MSG_DONTWAIT` with an offset); each channel keeps its own copy of the
  request body (the parser buffer is reused, `LinkFrame.cpp:84`); while a
  channel's request is unsent, pipe reads for it pause (USB NAKs; BLE: drop
  + `overflow` counter, as today).
- Pipe TX: one pending data frame + offset per session, plus a small control
  queue (PONG, HELLO_ACK, NET_ACK, NET_CANCEL, fail) sent at frame
  boundaries before data. S1's filler is part of the pending frame. `pump()`
  does not `recv` while a frame is pending (`_body`/`_tx` single buffers).
- `netService`: NET_REQ + NET_BODY become an offset state; `netOpen` still
  blocks its caller (NetJob) until sent.
- USB and BLE sessions serviced independently; the give-up time counted
  across passes. Counters kept.
- Verify: `applink busy` < 200 ms per window during a push through the app;
  rc_smoke over the link; no `app via none` flaps during pushes.

### S6 — supervisor and foreign stacks (E7)
- A supervisor on its own tick (an esp_timer callback that only notifies a
  small check, or the logbus task — not `net_svc`, which it watches):
  `W [sup] late task=<n> age=<ms> limit=<ms>` once per episode, `I [sup]
  task=<n> back after <ms>`; limits in `TaskPlan.h`: keys/hu/loop/knobs
  500 ms, car/ui 1 s, applink/ble_svc 3 s, net_svc 2 s (its OTA-critical
  limit, `Housekeeping.cpp:78`), netjob none.
- `[stk]` gains `nimble_host`, `arduino_usb_eve`, `arduino_events`, `usbd`,
  `wifi`, `tiT` (15-char names), `-` when absent.
- `[win]` context at each new minimum (S3's measurement): `at=` and the
  flags `job,http,ble<n>,join`.
- Guards: `W [mem] low …` when `[win] min` < 20 kB; `W [stk] low task=…` when
  < 1024 B free; once per episode.
- `FEATURE_TASK_WDT` (default 0) with the `esp_task_wdt_add` path; an env
  `t2can_wdt` builds it with 1 so it compiles in CI.

### S7 — passive service tasks
- `ble_svc`: block on its queue with the next deadline; no 20 ms poll while
  nothing is connected (behaviour of AMS/HID queues unchanged).
- `net_svc`: keep the 20 ms heap sampling (S3/S6 depend on it) — move it to a
  periodic esp_timer that only samples into atomics (free size every 20 ms,
  the largest block every 200 ms), then `net_svc` blocks to its next deadline
  and beats ≥ 1 Hz.
- Verify: wakes per window drop; `[win] min` still sampled at 20 ms; WiFi
  policy and BLE timings unchanged on the bench.

## Not in 1.12

`esp_task_wdt` reboot; NimBLE callback restructuring and NimBLE memory in
PSRAM; actor template / main.cpp split; the APK side of S1; knobs 1 kHz poll.

## Release check

All envs build; native tests; check_rules/check_pages. Bench mo-8428: OTA,
`[ota] CONFIRMED`, rc_smoke over WiFi and the USB link, a capture replay
(size per record), a paced spill with the stall measured, a held background
push under a synthetic heap squeeze and a user check that is not held. Then
`v1.12.0-rc1` for the car; the car checklist is each slice's "Verify".
