# Design: event tasks (T-2CAN production core)

> Design notes kept from development: the plan, its steps and the decisions
> taken on the way. Where they disagree with the code, the code is right.
>
> **Since then** (names below are the ones of 2026-10-09):
>
> | here | now |
> | --- | --- |
> | `AffaMailbox`, `MailboxIds` | `display/PanelOps` (any-task renders, tickets) |
> | `TwaiAffaLink`, `bus/TwaiBus`, the TX gate | `affa::TwaiLink` in the AffaDisplay library |
> | `UiPolicy` | `app/UiTask` |
> | `vehicle/RaiseTargets` | `proto/canbox/RaiseTargets` |
> | `bus/Mcp2518Bus` | `bus/Mcp2518Source` |
> | `CloudLink`, `WsStream` and `/ws`, board V and `src/link/`, `env:devkit`/`bare`/`boardv`/`ledtest` | deleted |
> | `PaneBuffers`, `App`/`AppContext`, `Telemetry`, `Supervisor`, `CrashLog` | never built as named, or folded into the tasks |
> | `docs/PLAN-nonblocking-callbacks.md` | deleted; its open items are mapped in §5 here |

Branch `refactor/event-tasks`. Written 2026-10-09. Planning only: no source file was changed to write this.

Target: LilyGO T-2CAN (ESP32-S3 N16R8, `env:t2can`). It sits on the car, drives the AFFA panel on CAN B (TWAI GPIO7/6) and listens to the car on CAN A (MCP2518FD, SPI CS10 SCK12 MOSI11 MISO13 INT8). It maps panel keys, knobs and car frames to Raise/canbox frames on the head-unit UART (Serial1 TX43/RX44, 38400 baud). The DevKit (`env:devkit`) is the bench and is flashed by OTA only. Its COM5 is never opened.

This plan superseded the open items of the nonblocking-callbacks plan (since deleted). A1-A8 are mapped to steps in §5.

Owner rules that every step obeys:
- `src/display` is never pruned. It is only extended.
- `FEATURE_USB_HID` stays a compiled, compile-checked sink option. UART is the main sink.
- The BLE stack copied from MeganeCAN (`bluetooth.cpp`, `apple_*_service.cpp`, `current_time_service.cpp`) is not restructured. We only change which task calls into it.
- No new WebSocket diagnostics. Diagnostics go through serial, LogBus and the flash log.
- Every image keeps OTA.
- Findings are reports. Removing code needs the list in §5 and the owner's go.

---

## 1. Current state and the problems it causes

Almost everything runs in `loop()` in `src/main.cpp` (2662 lines): loop task, priority 3, core 1, `delay(2)` per pass. One pass does the following, in order:
1. Drains the HU UART RX, without a bound.
2. Runs TWAI maintain.
3. Drains the AffaMailbox.
4. Calls `g_display->poll()`.
5. Drains panel keys into `KeyEngine::dispatch`, which runs all the sinks inline.
6. Runs about 300 lines of screen policy: media, notification, splash, pane compose, clock, popup-hide.
7. Heartbeat and BLE clock.
8. Knob drain.
9. `Mcp2518Bus::poll`, without a bound.
10. Sink ticks: release deadlines.
11. Emitter tick.
12. WiFi loop and cloud loop, including the OTA verdict.

Other tasks:

| Task | Priority | Core | Notes |
|---|---|---|---|
| knobs | 5 | unpinned | |
| net_svc | 1 | unpinned | stack 3072 |
| ble_svc | 1 | unpinned | |
| logbus | 1 | 0 | |
| cloud | 1 | 0 | |
| esp_http_server | **5** | unpinned | `HTTPD_DEFAULT_CONFIG`, above the loop |
| NimBLE host | | | |
| ACAN2517Handler | **16** | unpinned | 1 kB stack, created by the library |
| scan | 5 | | matrix builds only |

### 1.1 Problems, by evidence

**Keys**
- **K1. Keys wait behind the whole loop pass.** A knob detent waits for render, heartbeat and BLE (`main.cpp:2608` is drained late). A panel key waits until the next `poll()`. Today's `keywait` metric stamps the key at drain time, so it hides the TWAI queue wait (`AffaKeySource.cpp:38`).
- **K2. Sinks run synchronously on the producer's task** (`KeyEngine.cpp:42-47`):
  - `AmsKeySink` does 1 write-with-response per press, and 15 per volume press: 450 ms to 1.9 s (`BleKeySinks.cpp:38-43`, `apple_media_service.cpp:220`).
  - The macro lock is held across the sinks with `portMAX_DELAY` (`MacroEngine.cpp:293-345`).
- **K3. Four tasks dispatch at once.** `/api/test`, `/api/canbox/key`, `/api/macros/test` and macros run the sink chain on httpd (priority 5) concurrently with the loop. This races `CanboxUartSink::_pending` (a stuck or doubled release), `Hid gReleasePending`, and the AMS client against `ServiceAms` on ble_svc.
- **K4. `UartTx::writeKey` drops a key silently** after a 20 ms lock timeout, with no counter (`UartTx.cpp:14`). `writeAck` does the same.

**UART**
- **U1. Serial1 has no TX ring, so `availableForWrite()` is the free space in the 128-byte FIFO.** The vehicle gate `avail >= len+24` (`UartTx.cpp:37`) lets about 100 B (about 26 ms) of telemetry sit in front of a key or of the HU ack. The head unit allows 10 ms for the ack (`UartTx.h:20`).

**Panel**
- **P1. The AffaDisplay library has no locks, yet httpd and the cloud task read it, and httpd uninstalls TWAI under it.**
  - Reads: `WebUi.cpp:2304-2376` and the cloud state function.
  - Uninstalls: `/api/can/mode`, `listen`, `timing`, `selftest`, `loop` (`main.cpp:887-938`). These run while the loop sits in `twai_receive`/`twai_transmit`.
- **P2. A missing panel ACK leaves CAN B error-passive.** The controller then retransmits forever: about 1400 busErr/s (memory, 2026-09-22). That is ISR load on core 1, the key core. `maintain()` handles only BUS_OFF and STOPPED (`TwaiBus.cpp:145-156`).

**Car**
- **C1. Nothing turns a car frame into a signal.** `Vehicle::State::set` has no caller. CAN A only feeds the sniffer. The CAN→Raise mapping does not exist yet, but the Signal→Raise half (`Rav4Emitter`) does.
- **C2. The ACAN2517FD ring is unlocked between two cores.** `append()` runs on the library task (priority 16, unpinned). `remove()` runs on the loop in `receive()` (`ACAN2517FD.cpp:790-791`). `taskDISABLE_INTERRUPTS` masks only the local core.

**Shared state**
- **S1. About 40 raw global pointers are handed to httpd and mutated with no lock.** Among them: MediaScreen, MediaPane, KeyEngine, maps, CanboxUartSink, Rav4Emitter, Vehicle::State, CanSniffer, HuMediaSource, TwaiBus, and Cfg Strings. Some of these are real use-after-free risks: `std::string` and `String` reassignment in HuMediaSource, Cfg, WifiManager and CloudLink.
- **S2. `huRecord` (the HU ring) is written from every UART writer task, outside any lock.** `s_tapOwner` is clobbered by other tasks.

**Watchdog, OTA and logs**
- **W1. Nothing watches the key core.** No `esp_task_wdt_add`. Only IDLE0 is checked. `loopTaskWDTEnabled=false`.
- **W2. The OTA verdict runs inside `loop()`.** A hung loop never rolls back. The health check is WiFi plus heap only, so an image with dead keys or a dead panel is confirmed after 60 s.
- **L1. IDF logs never reach the flash log.** On T-2CAN, panic output goes to UART0, which shares pins with the head unit. The log rotates on every boot, so a boot loop erases the first crash. Coredumps can only leave through the cloud, and the cloud was never deployed.
- **L2. Flash erases stall both cores' caches** (no `SPI_FLASH_AUTO_SUSPEND`; `YIELD_DURING_ERASE` yields only between sectors). The 2 s LittleFS flushes therefore freeze keys, panel and CAN A. Not measured yet.

**Build**
- **B1. `env:native` pins AffaDisplay `#v1.0.0` (`platformio.ini:489`), but the targets use `symlink://../AffaDisplay-probe` (`:152`).** The `Version.h` build id is a fixed `"1.0.0"`, so logs from different flashes cannot be told apart.

Measured facts we rely on:
- DevKit idle free heap is about 80 kB (memory "heap is nearly gone").
- The board-V link runs about 460 fps with 0 CRC errors.
- A missing CAN B ACK looks like busErr ≈ 1400/s.

Everything else in §1.1 comes from code reading, not measurement. Step 1 adds the instruments.

---

## 2. Target architecture

### 2.1 Task table

`App::core(n)` returns 0 when `portNUM_PROCESSORS == 1` (C3), so one table works on all three chips. Stacks are per board in `src/app/TaskPlan.h`. The DevKit values are the tight ones; see the heap budget in §2.3.

| Task | Prio | Core S3 / ESP32 / C3 | Stack S3 / DevKit | Wakes on | Owns |
|---|---|---|---|---|---|
| `keys` | 7 | 1 / 1 / 0 | 4096 / 3072 | `KeyBus`: release queue first, then tap/press queue, then web queue. Timeout = exact next sink deadline, or 100 ms | KeyEngine dispatch, MacroEngine, AffaKeySource mapping, CanboxUartSink and its release table, UsbHidKeySink, ResistiveKeySink, **Hid press + service** (see Q2), the key-latency histogram, the key-routing apply |
| `hu` | 6 | 1 / 1 / 0 | 3072 / 3072 | `UartOut` queue with a 2-tick timeout, which is also the RX poll | Serial1 RX (RxParser, ack), Serial1 non-key TX (vehicle, raw, 0x90 replies), HuMediaSource, HU ring, junk batching |
| `knobs` | 6 | 1 / 1 / 0 | 3072 | `vTaskDelayUntil` 1 ms | Wire/PCF8574, LED rings. Posts Taps to KeyBus |
| `scan` (matrix builds only) | 6 | 1 / 1 / 0 | 3072 | 1 kHz | Matrix scanner, debouncer, encoder. Posts to KeyBus |
| **`loopTask` = panel** | 5 | 1 / 1 / 0 | 8192 (Arduino) | `twai_receive` into a one-frame stash. Wait = 1 tick while `busy()` or the mailbox is non-empty, else 2 ms | TwaiBus in Display role, AffaDisplay (`setPollOwner`), mailbox drain, resync, PanelStatus snapshot, ticket→request map, pane buffer release, TX gating, CAN bench ops |
| `car` | 4 | 0 / 0 / 0, **decided by measurement in Step 5** | 4096 | task notify from our MCP INT ISR (T-2CAN), or 2 ms (link, TWAI vehicle). Timeout 20 ms | Vehicle sources, MCP2518FD SPI, CanDecode, Vehicle::State writes, Rav4Emitter, sniffer, raw ring |
| `ui` | 2 | 1 / 1 / 0 | 6144 / 5120 | `UiQ` (completions, popup arms, scene requests) with a 50 ms timeout | MediaScreen, MediaPane compose, clock policy, notifications, splash, popup-hide, bring-up probe, render breaker, phase logging, 30 s stuck detection |
| `ble_svc` | 2 | 0 / 0 / 0 | 4096 | `AmsCmdQ` with a 20 ms timeout (replaces `vTaskDelay(20)`) | BleHub::Service, ServiceAms, **AMS command execution** (coalesced) |
| httpd | 2 | 0 / 0 / 0 | IDF | sockets | nothing. It posts, stages and reads snapshots only |
| `net_svc` (housekeeping) | 1 | 0 / 0 / 0 | 6144 (was 3072) | 20 ms | ElegantOTA, WsStream flush, WifiManager loop, heartbeat assembly, CloudLink loop and the OTA verdict, Supervisor |
| `logbus`, `cloud` | 1 | 0 | as today | as today | as today |
| ACAN2517Handler | 16 | lib | 1 kB | **never woken** (Step 5): we pass our own ISR, so its semaphore is never given | nothing |

Decisions that differ from the draft:
- **The Arduino loop task becomes the panel task.** It is already core 1 with an 8 kB stack. This saves a 6 kB task on the DevKit. Housekeeping moves to `net_svc`, pinned to core 0.
- **There is no UART event task.** At 38400 baud (3.8 B/ms) a 2 ms poll inside `hu` is enough, and it needs no global `ARDUINO_SERIAL_EVENT_TASK_*` flags, which would also hit the DevKit's Serial2 link.
- **Knobs run at priority 6, above the panel.** `KnobService.cpp:75-77` explains why: a starved 1 kHz sampler drops detents.
- **Only core-1 tasks (`keys`, `hu`) ever take the UartTx mutex.** A core-0 holder can be preempted by WiFi, lwIP or NimBLE, and priority inheritance does not lift it above those tasks. `car` and httpd post frames to `hu`.

### 2.2 Data flow

```
panel KeyCb ─┐                         ┌─> CanboxUartSink ─> UartTx (key lane, direct)
knobs ───────┼─> KeyBus ─> keys task ──┼─> UsbHid / Resistive / Hid (inline, non-blocking)
scan ────────┤  (rel q > tap q > web q)└─> AmsCmdQ ─> ble_svc (coalesced AMS writes)
web/macro ───┘
car frames ─> car task ─> CanDecode ─> State ─> Rav4Emitter ─> UartOut ─> hu task ─> UartTx (vehicle lane)
HU RX ─> hu task ─> RxParser ─> ack (immediately) / 0x90 ─> CarCmd ─> car / text ─> AffaMailbox
ui task ─> AffaMailbox (ReqId) ─> panel task ─> AffaDisplay ─> Completion ─> UiQ ─> ui task
panel task ─> PanelStatus snapshot <─ ui / httpd / cloud
httpd ─> Staged<T> configs, Request/Reply slots, queues; reads snapshots
```

### 2.3 Heap budget (DevKit is the binding case)

Stacks today: knobs 3k, net_svc 3k, ble_svc 4k, logbus 4k and loop 8k.

New stacks: keys 3k, hu 3k, ui 5k, car 4k (DevKit runs car only with the board-V link), plus 3k more on net_svc. That is about **+18 kB**. Idle free is about 80 kB today, which leaves about 62 kB. That is below CloudLink `FREE_MIN 65000`.

Mitigations:
- Measure first.
- If a fix is needed, use either of these, which are cheap:
  - Run `ui` on the DevKit with 4 kB, and compose the pane into a static buffer, not on the stack.
  - Re-size `FREE_MIN` per board in Step 6.
- The standing check (§4.0) records idle free and the largest block on the DevKit and **stops the step** if idle free is below 60 kB.

---

## 3. Concurrency contracts

### 3.1 Ownership rule

Every mutable object has exactly one owning task. Another task may only do one of these:
1. Post a typed message to the owner's queue, with zero wait and a drop counter.
2. Read a `Snapshot<T>` the owner publishes. This is a portMUX copy, the house pattern used by `g_notifyMux` and LogBus. **No seqlock**: the structs are under 128 B, and a seqlock reader can livelock on a unicore C3.
3. Stage a config with `Staged<T>`: a mutex, a dirty flag and two preallocated buffers. The owner calls `applyIfDirty()` with a **zero-wait try-take**, so it never blocks and never mallocs on the hot path.
4. Use a Request/Reply slot (§3.3) for web operations that need an answer.

Word-sized flags and counters may stay plain only when there is a single writer. Counters with several writers become `std::atomic<uint32_t>`.

### 3.2 Queues and mailboxes

| Channel | Producer(s) | Consumer | Item | Depth | Full policy |
|---|---|---|---|---|---|
| `KeyBus.rel` | all key producers | keys (drained first) | Release | 16 | never expected. On overflow the keys task's "down set" synthesises the release (§3.4) |
| `KeyBus.tap` | panel, knobs, scan | keys | Press / Long / Tap(press+release) / PanelKey(code, action) | 32 | drop the press and count it per origin. A Tap is one item, so its release can never be lost alone |
| `KeyBus.web` | httpd, macro run | keys | Event / Literal / MacroRun | 4 | drop and count. Web spam can never starve panel keys |
| `AmsCmdQ` | keys (BleKeyProxy) | ble_svc | {cmd, steps, atUs} | 8 | volume steps coalesce into a pending counter. Items older than 300 ms are dropped and counted |
| `UartOut` | car, httpd (raw), macro Message | hu | frame ≤ 36 B (`RaiseFramer::MAX_FRAME`) + kind | 16 | drop and count by kind. A 0x90 reply has a retry slot (§Step 5) |
| `CarCmd` | hu (0x90), httpd (pin, sendNow, sniff ops), keys (macro Message) | car | tagged union | 16 | drop and count |
| `AffaMailbox` | ui, keys (popup), hu (text), httpd, macro | panel | Req + ReqId | 16 (was 8) | drop and count. Media rows use a coalescing single slot outside the queue |
| `UiQ` | panel (Completion), keys (ArmPopup, SceneReq), httpd | ui | tagged union | 16 | drop and count. The ui task also times out every in-flight ReqId after 4 s |

All queues are created in `setup()` before any producer task starts. If a consumer is absent (BLE off, matrix off), the queue is still created, and the producer side drops and counts.

### 3.3 Locks that remain

- **`UartTx::_lock`**: taken only by `keys` and `hu`, both on core 1. It is held only for a non-blocking FIFO copy (µs). `LOCK_WAIT` stays at 20 ms as a safety net, and timeouts are counted (`keyDropped`, `ackDropped`, `vehDropped`).
- **HAL UART mutex**: the `hu` task reads only with `read(buf, min(available(), 512))` after `available()`, and calls `Serial1.setTimeout(0)`. It never calls `readBytes` with a timeout, and never a bare `read()` on an empty port. The HAL mutex is shared with writes (`esp32-hal-uart.c:581,599`). Step 2 adds a grep check to `tools/check_rules.py`.
- **`Staged<T>` mutex**: httpd may block on it. Owners only try-take.
- **AmsMediaSource mutex**: only `ui` takes it, with a 5 ms try-take. `ui` is never on the key path.
- **Request/Reply** uses `RR::Slot[4]` static slots, each `{gen, op, args, result, state, done semaphore}`, guarded by a portMUX. httpd takes a free slot, posts its index and generation to the owner, and waits up to 2 s. On timeout it marks the slot `abandoned` under the mux. The owner writes the result only if the generation matches and the slot is not abandoned. There are no pointers into the httpd stack.

### 3.4 Key contract (A3)

- `KeyMsg.atUs = esp_timer_get_time()` is taken at the source. For a panel key, `atUs` is the library's decode time. The keys task also records `pollGapUs` (the time since the previous `poll()` start) so the TWAI queue wait is visible.
- `Event.tMs` is the source time. A sink computes a release deadline as `max(e.tMs + pulse, now + MIN_GAP_MS)` with `MIN_GAP_MS = 20`. A queued key therefore never gets a shortened or already-due release. The contract is written in `IKeySink.h`.
- The keys task keeps a down set per origin and action. A Press whose Release has not arrived within `STUCK_MS` (2 s for UART, the sink's own pulse for detents) gets a synthesised Release, and `stuckSynth` is counted.
- The latency histogram records two numbers: `srcToWriteUs` (atUs to FIFO copy) and `wireUs = srcToWriteUs + inflightBytes*260`.

### 3.5 UART lanes (`src/sinks/UartLanes.h`, portable, tested)

`inflight = 128 - availableForWrite()`. Priority: **ack > key > reply(0x90) > vehicle/raw**.

- Ack and key: always written.
- A vehicle or raw frame is written only when `inflight == 0` **and** `RxParser` is not in the middle of a frame.
- The worst ack or key delay is therefore one vehicle frame: `L_max * 260 µs`. Step 2 measures `L_max` across the Rav4 table. If it exceeds 24 B (6.2 ms), the gate becomes `inflight == 0 && len <= 24`, or the longest messages go out only when no HU frame arrived in the last 20 ms.
- Never add a TX ring buffer: a ring puts telemetry in front of keys.
- With `hu` as the only non-key writer, the lane policy is decided in one task. `keys` only ever writes the key lane.

### 3.6 Panel ownership

- `g_display` and `g_carminat` are removed from global scope. They are reachable only through `PanelOwner::lib()`. In debug builds (`-D AFFA_OWNER_ASSERT=1`, on for devkit) `lib()` asserts `xTaskGetCurrentTaskHandle() == panelTask`. The library's `setPollOwner` guards only `poll()`, so `foreignPolls=0` alone does not prove render ownership.
- `onAffaComplete`, `onAffaSync`, `affaTap` and the log sink always run on the panel task.
- The ticket protocol works like this:
  - The panel maps `lastEnqueued()` to the ReqId right after each render call.
  - If a call returns `kNoTicket` (BadArgument or TooLong, `AffaTx.cpp:194-198`), the panel **synthesises** `Completion{reqId, lastResult()}` and releases any pane slot on the panel side.
  - A coalescing Aborted completion (`AffaTx.cpp:290`) fires inside the render call. It still maps, because the old ticket was mapped at its own drain.

---

## 4. Steps

Six steps, which match the user's "all six steps". The draft's Steps 0 to 6 are folded together:
- Scaffolding goes into Step 1.
- The hu task moves before the panel move, as the critique asked.
- The main.cpp split and hardening share Step 6.

### 4.0 Standing check (every step, every commit series)

1. Build every env the step touches:
   ```
   pio run -e devkit -e t2can -e c3 -e direct -e bench -e min
   pio test -e native
   ```
   - Add `-e bare -e boardv -e ledtest` only when `platformio.ini` changes. Their source filters compile none of the touched files.
   - **`env:direct` must build every step.** It has no AffaDisplay and no `src/display/`.
   - From Step 2 on, also build `t2can_usbhid`.
2. The native `build_src_filter` (`platformio.ini:544`) is an allow-list. **Every new portable `.cpp` is added to it in the same commit.**
3. Flash the DevKit by OTA only. The OTA trial must confirm.
4. Over 10 minutes of bench use, check the flash log (`/log/N.log`), not WebSockets:
   - no STALL and no panic;
   - no `[task]` stack low-water under 512 B;
   - DevKit idle free ≥ 60 kB, and the largest block is recorded.
5. Do not edit library files (AffaDisplay-probe, ACAN2517FD, NimBLE, PsychicHttp). The libdeps cache was hand-patched once; do not repeat it.

### Step 1: Runtime scaffolding, visibility, and the safe cheap fixes

**Goal.** Make everything after this step measurable. Lower httpd. Base the OTA verdict on liveness. No key-path behaviour changes.

**Files**
- Create:
  - `src/app/TaskPlan.h`: the priority, core and stack table from §2.1, with the `core()` clamp.
  - `src/app/TaskRegistry.{h,cpp}`: 16 entries of `{name, handle, lastAliveMs, wakes, busyMaxUs, critical}`. Each owner writes its own entry; the heartbeat reads them lock-free.
  - `src/app/Rt.{h,cpp}`: `App::spawn(spec)` wraps `xTaskCreatePinnedToCore` and registers the task. The WDT flag is accepted but ignored until Step 6.
  - `src/utils/Snapshot.h` and `src/utils/Staged.h` (portable).
  - `src/utils/LatencyHist.h`: portable log2 µs buckets with p50, p99 and max.
  - `src/app/RequestReply.{h,cpp}`: the slots from §3.3. The generation and abandon logic is a portable class.
  - `tools/version.py`: git short hash, a dirty flag that **ignores `src/net/gen/PageGz.cpp`**, and the build time, written as `-D MEGAOPEN_BUILD`. It returns early when the platform is native.
  - `test/test_runtime/`: Snapshot, Staged (zero-wait apply, buffer swap), LatencyHist and RR slot generation and abandon.
- Modify:
  - `platformio.ini`:
    - `extra_scripts = pre:tools/gen_pages.py, pre:tools/version.py` (line 117).
    - **Align `env:native`'s AffaDisplay with the targets.** Use the same `symlink://../AffaDisplay-probe` (lines 152 and 489).
    - Add `env:t2can_usbhid`, which extends t2can with `FEATURE_USB_HID=1` and `ARDUINO_USB_MODE=0`. It is a compile check only.
  - `src/Version.h`: use `MEGAOPEN_BUILD`.
  - `src/net/WebUi.cpp` (around lines 96-106, before `listen`): `_server.config.task_priority = 2; _server.config.core_id = 0;`. PsychicHttp keeps a public `config = HTTPD_DEFAULT_CONFIG()` (`PsychicHttpServer.cpp:21`).
  - `src/main.cpp`:
    - Register loop, net_svc, ble_svc, knobs and logbus in the registry.
    - Add a `[task]` heartbeat line, `[boot] build=… reason=… boots=… crashes=… psram=…`, and `[stall]`, the largest gap between consecutive loop wakes measured with `esp_timer` (it shows flash-erase freezes).
    - The OTA verdict health becomes "registry critical tasks alive within 2 s **and** heap OK". WiFi stays in the predicate because it is today's behaviour. Peripherals do not gate.
  - `src/sinks/UartTx.{h,cpp}`:
    - `writeKey` and `writeAck` return bool.
    - Add the `keyDropped`, `ackDropped` and `vehDeferred` counters, with `_deferred++` moved under the lock.
    - Report them in `[hu]`.
  - `src/keys/KeyEngine.{h,cpp}`:
    - `MAX_SINKS` goes from 4 to 6.
    - `addSink` overflow is logged.
    - `_events` and `_filtered` become `std::atomic`.
  - `src/main.cpp:onKeyEvent` and `onAffaKeyReport`: the `[key]` line goes through `LogBus`/`Log::`, not `g_ws.printf`. This takes the CloudLink tap memmove out of the key path.
  - `src/bus/TwaiBus.cpp:59-66`: `Serial.printf` becomes `Log::`.
  - `src/knobs/KnobService.cpp`: create the task through `App::spawn`, pinned to core 1, priority 6.

**Contract.** Registry entries are single-writer. Nothing else changes.

**Risks**
- With httpd on core 0, its busy-wait diagnostic routes now run next to IDLE0, which the TWDT watches with panic already on (`CONFIG_ESP_TASK_WDT_PANIC=1`). The routes are `/api/can/level` (80 ms), `/api/can/ecc` and `/api/can/txwatch` (400k samples). Add `esp_task_wdt_reset()`/`vTaskDelay(1)` every 50 ms inside those loops, or cap them at 1 s.
- Raising knobs to priority 6 on core 1 above the loop: knob work is about 0.2 ms per ms of I2C, which the loop can tolerate.

**Acceptance**
- The standing check passes.
- `[boot] build=<hash>` appears in the flash log, and `/api/health` shows the build id.
- `[task]` lists every task with stack low-water, wakes and busyMax.
- `[stall]` is reported. Record the value during a log rotate: this is the L2 baseline.
- `pio test -e native` passes against the probe library.
- `/api/can/level` still works.

### Step 2: The keys task, UART lanes and the hu task

**Goal.** One dispatcher. Keys never wait for render, BLE, web or telemetry. Acks hold the 10 ms window. This delivers the "keys never wait" promise before anything moves the panel.

**Design**
- **KeyBus** (`src/app/KeyBus.{h,cpp}`) holds the three queues from §3.2. The policy part, `KeyRouter` (ordering, down set, stuck synthesis and deadline rule), is a portable class with no FreeRTOS. `KeyMsg` carries `PanelKey{uint8_t code, uint8_t action, atUs}` as plain bytes, with no `affa::Key` and no `<AffaDisplay.h>` include. This is A6, and it keeps `env:direct` building.
- **Producers**
  - `onPanelKey`, inside `poll()`, posts PanelKey. `g_panelKeyQ` and `drainPanelKeys` are deleted.
  - Knob `emitThunk` posts one **Tap** item, or Press and Release items into the right queues. KnobService `_q` and `drain()` are deleted.
  - Matrix `KeyEngine::poll` gets an `EmitFn` hook and posts to KeyBus (half of A5). **Remove `g_macros.tick()` from `scanTask` (`main.cpp:1488`).**
  - Web routes post to `KeyBus.web` and return `202 {queued:true}`: `/api/test` (as a Tap), `/api/canbox/key` (Literal) and `/api/macros/test` (MacroRun). The UI only checks `r.ok` (`WebAssets.h:385-386`).
  - `/api/canbox/raw` posts to `UartOut`, with frames up to 36 B. It never takes the UART mutex from httpd.
- **Keys task loop**
  1. `applyIfDirty()` for routing, AffaMap, KeyMap and the macro table. Use try-take; the buffers are preallocated.
  2. Receive with this priority: rel, then tap, then web. The timeout is the exact next deadline.
  3. Handle the message: AffaKeySource mapping, the report (log line and `postPopup` to the mailbox; the popup timer arm goes as `ArmPopup` to the popup owner), the macro filter, `dispatchRaw`, then the sinks. The UART sink stays first.
  4. Run every deadline: `g_keySink.tick`, `g_usbKeys.service`, `g_resSink.tick`, `g_macros.tick` and `Hid::service` (see below).
  5. Call `registry.alive()` and record the histogram.
- **Macros.** Only the keys task touches MacroEngine. `g_macroLock` is deleted. The web `/api/macros` POST and defaults go through `Staged<MacroTable>`, which fixes the unlocked writes at `WebUiMacro.cpp:203-216`. Effects:
  - `Action` → `dispatchRaw`, which runs inline on the keys task.
  - `KeyCode` → sendKey, inline.
  - `Raw` / `Message` → `UartOut` / `CarCmd`. Until Step 5 a `Message` is a one-shot `SendNowReq` slot that `hu` consumes, and `hu` calls `g_emitter.sendNow`. The emitter tick moves to `hu` in this step, so it has a single owner (see below).
  - `Text` / `Popup` → mailbox, plus `ArmPopup`.
  - `Pane` → a `SceneReq` one-shot slot (atomic) that the current pane owner consumes: the loop until Step 4, then `ui`.
- **BLE sinks**
  - **AMS** is registered as `BleKeyProxy`. It posts to `AmsCmdQ`. ble_svc blocks on `AmsCmdQ` with a 20 ms timeout and then runs `BleHub::Service()`. It executes **one** AMS write per iteration and coalesces volume steps into a counter. Items older than 300 ms are dropped. The AMS writes then run on the same task as `ServiceAms`, which closes the race with ServiceAms. `BleKeySinks.cpp` is MegaOpen code and is reshaped. `apple_media_service.cpp` is not touched.
  - **HID** runs press and service on the keys task. `Hid::press` notifies without waiting for a response. `BleHub::Service` stops calling `Hid::service()` when `Hid::externallyServiced()` is true. That is a one-line change in `BleHub.cpp`, which is MegaOpen's hub, not a MeganeCAN file: **owner question Q2**. If the owner refuses, the fallback is HID on ble_svc, with HID items handled before AMS and `Hid::service()` also called between AMS writes.
  - Routing masks for AMS and HID are staged to the task that executes them: ble_svc for AMS, keys for HID.
- **The hu task** (`src/app/HuTask.{h,cpp}`)
  - It owns: the Serial1 RX drain (bounded at 512 B per wake, using the HAL read rule from §3.3), RxParser, the **immediate ack**, `onHuFrame`, HuMediaSource (with a mutex `snapshot()` for other readers, like AmsMediaSource), the HU ring (now single-writer except uartTap; see below), and junk batching.
  - It also owns: `huDrainSerial` formatting (rate-limited), `UartOut` writes through the vehicle lane, `g_emitter.tick`, and the 0x90 responder `sendByCmd`. The emitter has one owner until Step 5 moves it to `car`. Web emitter setters become `Staged<EmitterCfg>`; `sendNow` is a request slot.
  - `uartTap`/`huRecord` gets a portMUX, because `keys` and `hu` both write the UART.
  - The 420-byte HU log line goes through `Log::`.
- **Popup timer.** `g_popupUntilMs` and `g_glassHeldUntilMs` get one owner, the loop (the render policy) until Step 4. Writers post `ArmPopup{ms}` and `ReleaseGlass` to a small one-shot slot set. There are no more direct writes from keys or httpd.
- **USB HID** stays a fast inline sink on the keys task (`ready()`-gated). It is compile-checked by `env:t2can_usbhid`.

**Files**
- Create: `src/app/KeyBus.{h,cpp}`, `src/keys/KeyRouter.{h,cpp}` (portable), `src/app/KeysTask.{h,cpp}`, `src/sinks/BleKeyProxy.{h,cpp}`, `src/sinks/UartLanes.h` (portable), `src/app/HuTask.{h,cpp}`, `src/app/OneShot.h` (the atomic request slots), `tools/check_rules.py` (forbidden-call grep: `readBytes` in HuTask, `g_ws.printf` in the keys/hu/panel task files, `portMAX_DELAY` in those task files).
- Modify:
  - `src/keys/KeyEngine.{h,cpp}` (EmitFn), `src/keys/IKeySink.h` (tMs contract), `src/keys/AffaKeySource.{h,cpp}` (plain code in, atUs), `src/keys/MacroEngine.{h,cpp}` (no lock, Staged table).
  - `src/sinks/CanboxUartSink.cpp` (deadline rule), `src/sinks/UartTx.{h,cpp}` (lanes), `src/sinks/BleKeySinks.cpp` (coalescing worker side).
  - `src/ble/HidRole.{h,cpp}` and `src/ble/BleHub.cpp` (the Q2 one-liner).
  - `src/knobs/KnobService.{h,cpp}` (post function; delete `_q`/`drain`), `src/media/HuMediaSource.{h,cpp}` (snapshot).
  - `src/main.cpp`: remove from `loop()` the HU drain, `huDrainSerial`, the panel key drain, the knob drain, the sink ticks, the emitter tick and `g_macros.tick`. `bleServiceTask` reads `AmsCmdQ`. Start `keys` and `hu`.
  - `src/net/WebUi.cpp` and `src/net/WebUiMacro.cpp` (post or stage instead of call).
- Tests:
  - New `test_keybus` (KeyRouter): queue priority, Tap atomicity, stuck synthesis, the deadline rule, UART sink first, MAX_SINKS overflow.
  - New `test_uartlanes`: ack > key > reply > vehicle; a vehicle frame is never written with inflight > 0 or mid-RX-frame; worst wire delay ≤ `L_max*260 µs`.
  - Extend `test_macro` (no lock; effects become requests), `test_keysource` (plain code) and `test_humedia` (snapshot).

**Contract**
- `keys` owns KeyEngine, MacroEngine, CanboxUartSink, UsbHid, Resistive and HID.
- `ble_svc` owns AMS.
- `hu` owns Serial1 RX, non-key TX, the emitter (for now) and HuMediaSource.
- UartTx is shared between core-1 tasks only.
- The loop still owns the panel and all render state.

**Risks**
- The keys task at priority 7 preempts the loop mid-render. Every effect that touches loop-owned state is a request slot (listed above). Grep for direct `g_pane.`, `g_popupUntilMs`, `g_emitter.` and `g_media3.` writes outside their owner, and add the patterns to `check_rules.py`.
- HuMediaSource readers on httpd copy `std::string` under the new mutex. Keep the hold short (a copy only).
- KeyBus depth against a fast encoder spin: watch the `drops` per origin.

**Acceptance**
- The standing check passes, including `env:direct`, `t2can_usbhid` and `check_rules.py`.
- `[key] n= p50= p99= max= wireMax= pollGapMax= drops(tap/rel/web)= stuckSynth=` reports, on the DevKit, **panel/knob `srcToWrite` p99 < 3 ms with AMS routed to VOLUME, a phone connected and the pane animating**. `wireMax` and `pollGapMax` are reported, not gated: the panel still polls on the loop.
- `[hu] acks= ackLateMax(wire)= <8ms keyDropped=0 ackDropped=0 vehDeferred= Lmax=`.
- A 15-step AMS volume burst no longer shows in `[key] max`. HID releases arrive 30-50 ms after the press, measured on the phone or head unit.
- The `[loop] keywait` metric is gone and replaced by `[key]`.

### Step 3: The panel task (the loop task becomes the sole AffaDisplay owner)

**Goal.** `poll()` is RX-driven. Housekeeping leaves the panel's task. The web stops touching the library and the TWAI driver. The render policy stays inline for one more step, so this step is behaviour-identical.

**Design**
- **Housekeeping moves to `net_svc`**, pinned to core 0 with a 6 kB stack. It runs the WiFi loop, heartbeat assembly (with `WiFi.SSID()` and the String work at most once a minute), CloudLink loop and the verdict, and sampleHeap. `loop()` keeps the panel pump plus the existing inline render policy, moved verbatim into `UiPolicy::serviceInline()`.
- The loop task is raised to priority 5. It calls `setPollOwner(loopTaskHandle)` and registers as critical.
- **Wake.** `TwaiAffaLink` gets a one-frame stash. Each pass calls `twai_receive(&m, wait)`. On success the frame is stashed and `poll()` runs, and `recv()` returns the stash before `recvOne`. `wait` is 1 tick while `busy()` or while `uxQueueMessagesWaiting(mailbox) > 0`, otherwise 2 ms. The library's shortest deadline is about 200 ms. This replaces `delay(2)` and wakes on a panel key frame directly. The stash is cleared on driver reinstall.
- **TX gating (P2).** The panel task reads `twai_get_status_info` once per 100 ms. If the controller is ERR_PASSIVE, or `tx_failed` rises, with **no RX for 2 s**, it calls `twai_clear_transmit_queue()` and `setTxEnabled(false)`. It re-probes with exponential backoff (1, 2, 4 … 30 s). `[panel] txGated= reinstalls=` reports it.
- **The web becomes a client**
  - `/api/can/mode`, `listen`, `timing`, `selftest` and `loop` become Request/Reply ops that run on the panel task. They live in `src/diag/CanBench.{h,cpp}`, moved from `main.cpp:887-938`.
  - `/api/can/level`, `pintest`, `pads` and `txwatch` require `?arm=bench` and refuse while the panel is registered or knobs are live.
  - `/api/can/alerts` stays a read-only status. It stops calling `twai_read_alerts`, which consumes alerts.
  - `fillDisplay`, `/api/health`, `/api/media` `busy()` and the cloud state function read `Snapshot<PanelStatus>`: synced, busy, registered, phase, sessionsLost, lastLossReason, queued, lastResult, linkHealth, lastRendered, failStreak, the delivery counters, foreignPolls, pollLateMaxUs and queueDropped. The panel task publishes it after each poll.
  - `g_resyncRequest` and `g_clearRequest` become mailbox ops `Resync` and `Clear`.
- `serviceMediaScreen` takes the AMS mutex with a 5 ms try-take and skips on failure. It still runs inline here; Step 4 moves it off.
- In the Vehicle role (DevKit `canRole=vehicle`) the panel pump is skipped and the loop only runs the vehicle TWAI source until Step 5.

**Files**
- Create: `src/app/PanelOwner.{h,cpp}` (the `lib()` accessor with the owner assert, the stash wiring, TX gating, status publishing), `src/app/UiPolicy.{h,cpp}` (the block from `main.cpp` ~2108-2430, unchanged logic), `src/display/PanelStatus.h` (a POD, display-only, wrapped in `#if FEATURE_DISPLAY_CAN`), `src/diag/CanBench.{h,cpp}`, `src/app/Housekeeping.{h,cpp}`.
- Modify: `src/display/TwaiAffaLink.h` (stash), `src/display/AffaMailbox.{h,cpp}` (Resync and Clear ops, atomic counters, depth 16), `src/bus/TwaiBus.{h,cpp}` (`_installed` atomic, `clearTx`), `src/main.cpp`, `src/net/WebUi.cpp`.
- All panel and ui files are gated with `#if FEATURE_DISPLAY_CAN` so that `env:direct` builds.

**Contract.** The loop (panel) task is the only caller of any `AffaDisplayBase` method and of `twai_*` in the Display role. Everyone else posts to the mailbox or reads `PanelStatus`.

**Risks**
- At priority 5 the inline compose can still take a few ms. Knobs are at 6 and keys at 7, so they are unaffected. `ui` does not exist yet.
- Moving the verdict to net_svc: if net_svc dies, the board does not reboot. The Supervisor in Step 6 covers this; in this step the registry liveness covers it.
- The DevKit heap: watch it.

**Acceptance**
- The standing check passes.
- `[panel] foreignPolls=0 pollLateMax<15ms txGated=0 reinstalls=0`.
- The panel registers within the usual time after a cold boot.
- `/api/can/selftest` returns a result from the web.
- Key popups still appear.
- `[key] pollGapMax` drops to about 2-3 ms.
- **Unplug test:** with the panel unplugged and the box powered, `txGated=1` appears within 2 s and `[key] p99` is unchanged. This needs the T-2CAN, or a DevKit with CAN B wired.
- In a debug build the owner assert never fires.

### Step 4: The ui task (the policy leaves the poll owner)

**Goal.** The panel task only pumps. All composition and policy run at priority 2 and only post.

**Design**
- **Mailbox ids.** `post()` returns `ReqId` (uint16, never 0). The panel maps tickets to ReqIds when it drains, and `onAffaComplete` pushes `Completion{reqId, result}` to `UiQ`. On `kNoTicket` the completion is synthesised (§3.6). This follows the AffaTask `TxRequest` pattern, and `AFFA_ENABLE_TASK` stays 0, because its Op set lacks NavBitmap, Raw and fullscreen.
- **New ops**
  - `SetTimeTracked`: the clock retry keys on the ReqId.
  - `NavBitmap{slot}`: `PaneBuffers` has two 288-byte slots. `ui` composes into the free slot and posts its index. **The panel releases the slot only on that request's completion**, synthesised ones included. That is the borrow rule for `enqueueSplit`.
  - `HidePopup`.
  - `MediaRows`: a coalescing single slot, so a newer repaint overwrites an unsent one.
- **UiTask** runs:
  - serviceMediaScreen and fillMediaScreen;
  - notifications;
  - splash;
  - now-playing repaint and throttle;
  - the pane carousel, step and compose;
  - the clock policy;
  - popup-hide (it now owns `g_popupUntilMs` and the glass hold, and consumes the ArmPopup and SceneReq slots);
  - the bring-up probe;
  - phase and registration logging;
  - 30 s stuck detection, which posts `Resync`;
  - the render breaker, reading `failStreak` from PanelStatus.

  Every in-flight ReqId times out after 4 s.
- **Web settings**
  - `g_media3`/`g_pane` web edits, and the `g_mediaAuto`, `g_paneAuto`, cycle, speed, splash and `autoTime` flags, become `Staged<MediaSettings>`.
  - `/api/media/paint` and the previews compose on a **copy** inside the handler, and the live objects are never touched.

**Files**
- Create: `src/app/UiTask.{h,cpp}` (UiPolicy moves here and becomes posting-only), `src/display/PaneBuffers.{h,cpp}`. Put `src/app/MailboxIds.{h,cpp}` (the portable ticket↔req table) **outside `src/display/`**, so the native allow-list and `env:direct` stay simple.
- Modify: `src/display/AffaMailbox.{h,cpp}` (ids, ops, coalescing slot, completion hook), `src/app/PanelOwner.cpp` (remove `serviceInline`), `src/main.cpp` (callbacks route to UiQ), `src/net/WebUi.cpp` (Staged media, copy-compose).
- Tests: new `test_mailbox` (id issue, mapping, coalesced Aborted maps, kNoTicket synthesises and releases the slot, 4 s timeout); extend `test_pane` and `test_media`.

**Contract.** The panel owns the library, tickets and slot release. `ui` owns media, pane, clock and popup state. They talk only through the mailbox (ui→panel), UiQ (panel→ui) and PanelStatus.

**Risks**
- Losing the one-pane-in-flight limit: covered by the tests above.
- A UiQ overflow drops completions, and the 4 s timeout recovers it.
- Media staleness from the coalescing slot is intended.

**Acceptance**
- The standing check passes.
- `[panel] busyMax<2ms`.
- `[ui] busyMax` is reported, and the AMS mutex wait appears only there.
- The pane carousel, clock and notifications behave as before on glass. Without glass, check `panesSent` against `panesFailed` and the clock completions in `/api/display`.
- `[key] p99` holds while the pane animates.

### Step 5: The car task and the CAN→Raise mapping

**Goal.** CAN A (or board V, or vehicle TWAI) is decoded into `Vehicle::State` on its own task. The emitter turns State into Raise frames through the hu task.

**Design**
- **Sources are chosen at boot, not at build time.** `IVehicleSource { begin(); size_t drain(Frame*, size_t max); stats(); }`. The car task holds a **list** built from the CAN role and the features:
  - `Mcp2518Source` on T-2CAN (FEATURE_CAN2);
  - `LinkSource` on the DevKit with FEATURE_LINK;
  - `TwaiVehicleSource` when `canRole=vehicle`.

  The DevKit can run Link and TWAI vehicle together. `applyCanRole` stays: it is called at boot (`main.cpp:1703`). In the Vehicle role the panel pump does not run.
- **MCP2518FD without a cross-core race, and with no library edit.** This has been verified against ACAN2517FD 2.1.16:
  - `begin()` always creates `ACAN2517Handler` (`ACAN2517FD.cpp:505`, outside the `mINT != 255` check), so INT=255 does not remove it. INT=255 also makes `receive()` run `isr_poll_core()` on every call (:794-796) and needs a null ISR (:234). **INT=255 is rejected.**
  - Instead, keep INT=8 and pass **our own** ISR to `g_can.begin(s, carIsr)`. The library attaches it FALLING (:509). `carIsr` does only `vTaskNotifyGiveFromISR(carTask)`. The library's `isr()` is never called, so `mISRSemaphore` is never given and the handler task blocks forever: 1 kB of RAM and 0 wakes.
  - On each wake the car task calls the public `g_can.isr_poll_core()` once (`ACAN2517FD.h:216`), then `receive()` up to 64 frames. With INT≠255, `receive()` only touches SPI to re-enable the RX interrupt when the ring was full. All SPI and ring access is then on one task.
  - Optional RAM recovery: `vTaskDelete(xTaskGetHandle("ACAN2517Handler"))` after `begin()`. Only do this if `end()` is never called, because `end()` deletes the stored handle (:559-562). **Not done by default.**
  - `errorCounters()` and the diagnostic reads become CarCmd Request/Reply.
- **Measure the interrupt masking before fixing the core.** `isr_poll_core` holds `taskDISABLE_INTERRUPTS` across its whole FIFO drain (:891-933). `[car] maskMaxUs` is measured with `esp_cpu_get_ccount` around the call. If it exceeds 200 µs, or BLE connections drop while CAN A is busy, the car task moves to core 1 at priority 3, below `ui`. Step 5 acceptance decides this, and the result is written into `TaskPlan.h`.
- **Per wake:** drain up to 64 frames. If more remain, `taskYIELD` and continue. Each frame goes through:
  1. `CanDecode::apply(frame, state, now)`: a sorted-id binary search.
  2. The raw ring, under a portMUX.
  3. The sniffer, only when `sniffMode` is on; it is off by default on t2can. httpd reads it through a Request/Reply snapshot.
- **CanDecode** (`src/vehicle/CanDecode.{h,cpp}`, portable):
  - Rows are `{canId, startByte, numBytes, shift, mask, mul, div, add, Sig, flags(LATCH, HOLD_MS, KEY)}`, following `docs/megane-can-ids.md:16-18`. The defaults are compiled in from that document (`decode_defaults.inc`).
  - **Adding a signal means adding a row. Adding a Raise message means adding a builder plus a row in `Rav4Emitter`.**
  - The NVS JSON override and the `/api/decode` editor are deferred. The compiled table plus OTA is enough for now.
  - The `KEY` flag posts a `KeyMsg` to KeyBus, so steering-wheel keys from the car use the same key path. The table has no such rows yet.
- **Vehicle::State.** The car task is the only writer. Readers (httpd) use `Snapshot<StateView>`. Web pins become `CarCmd::Pin`.
- **Rav4Emitter moves from `hu` to `car`.**
  - `tick` runs on every wake or timeout. Only messages that are due are built and hashed, if `[car] busy` shows the 22-message rebuild.
  - Frames go to `UartOut`; `hu` writes them through the vehicle lane.
  - The 0x90 request: `hu` posts `CarCmd::SendByCmd`. The reply comes back as a `UartOut` item of kind Reply, which has a retry-until-sent slot and the reply lane priority.
  - The emitter **stays disabled by default** until the table is verified on the car.

**Files**
- Create: `src/app/CarTask.{h,cpp}`, `src/bus/IVehicleSource.h`, `src/bus/Mcp2518Source.{h,cpp}` (replaces Mcp2518Bus), `src/link/LinkSource.{h,cpp}`, `src/bus/TwaiVehicleSource.{h,cpp}`, `src/vehicle/CanDecode.{h,cpp}`, `src/vehicle/decode_defaults.inc`.
- Modify: `src/vehicle/VehicleState.{h,cpp}`, `src/vehicle/CanSniffer.{h,cpp}` (mode, snapshot), `src/proto/canbox/Rav4Emitter.{h,cpp}` (Staged tuning), `src/app/HuTask.cpp` (emitter out, Reply slot in), `src/link/LinkRx.cpp` (filter and sim through CarCmd), `src/main.cpp` (`busRx` and the `#if` source blocks leave), `src/net/WebUi.cpp` (signals, emitter, sniffer and link routes become CarCmd or Staged), `docs/megane-can-ids.md` (the table is now compiled in).
- Delete `src/bus/Mcp2518Bus.{h,cpp}` once `Mcp2518Source` replaces it.
- Tests: new `test_decode` (extraction, sign, latch/hold, unknown id, KEY flag); extend `test_vehicle`, `test_proto` (Reply priority, emitter ordering) and `test_sniffer` (mode off = no ingest).

**Contract**
- `car` owns the sources, SPI, decode, State writes, the emitter, the sniffer and the raw ring.
- `hu` owns all non-key UART TX.
- httpd uses only Staged or Request/Reply.

**Risks**
- `maskMaxUs` and the core choice (above).
- The MCP path is provable only on the T-2CAN, which is on order. On the DevKit, LinkSource and the TWAI vehicle source are testable.
- The decode defaults are partly hypotheses, as the document says.

**Acceptance**
- The standing check passes.
- DevKit with board V: `[car] fps≈460 drainMax≤64 busyP99<1ms ringOvf=0`.
- With the emitter on, `[hu] ackLateMax(wire)<8ms` and `[key] p99` change by no more than `L_max*260 µs`.
- On T-2CAN, on arrival:
  - `[car] fps` matches the bus.
  - `fdDropped` is counted.
  - `ACAN2517Handler wakes=0`.
  - `maskMaxUs` is recorded and the core is decided.
  - A decoded signal (speed or rpm) appears on `/api/signals` and in Raise frames on the head unit.
  - `[car] ovf=0` during a log-file rotation.

### Step 6: Production hardening, and main.cpp becomes a wiring file

**Goal.** In the car the box recovers on its own, leaves evidence, and is identified by build. `main.cpp` shrinks to construction and wiring.

**6a. Code motion**
- Create `src/app/App.{h,cpp}`: construction in today's order, feature gating, queue creation, task start.
- Create `src/app/AppContext.h`: one struct of owner handles, queues and snapshot accessors handed to WebUi. It replaces the ~40 `attach*` raw pointers.
- Create `src/app/Telemetry.{h,cpp}`.
- `main.cpp` becomes `setup(){App::begin();} loop(){App::panelPass();}` and should be under 150 lines.
- Apply the removals in §5 that belong to Step 6.

**6b. Watchdog**
- **Trial day.** The Supervisor in net_svc logs any critical task whose `lastAliveMs` is older than 2 s, as `[sup] late task=…`. Do **not** call `esp_task_wdt_init(…, panic=false)`: that would also downgrade the existing IDLE0 panic.
- **Then.** `App::spawn` calls `esp_task_wdt_add` for keys, hu, the panel/loop, car, ui and knobs, and each feeds on every wake. All of them wake at least every 100 ms by design. The TWDT is already 5 s with panic, so a hang leads to panic, coredump and reboot. **While an image is PENDING_VERIFY, that reboot rolls it back.**
- ble_svc is **not** subscribed, because GATT discovery can block for seconds. A stale stamp older than 30 s is logged and never reboots the box.

**6c. OTA verdict**
- Confirm on: all critical tasks alive, no panic, and heap above the per-board thresholds, all held for 60 s.
- Absent peripherals are **neutral and logged, never gating**: no panel ACK, no CAN A traffic, a sleeping head unit, or OTA done with ACC off. Otherwise a good image would roll back in a parked car.
- Re-size `HEALTH_HEAP_MIN`, `FREE_MIN` and `LARGEST_MIN` per board. The S3 numbers come from the Step 1-5 telemetry.

**6d. Logging and crash evidence**
- Route the IDF `esp_log` tap into LogBus as well (flash and serial). Lines that originate on the logbus task itself are dropped, using a per-task TLS guard that replaces `s_tapOwner` (which also fixes S2's guard).
- Remaining `g_ws.printf`/`g_ws.line` call sites (`[ancs]`, clock, routing and settings) become `Log::`.
- Rotate on boot **only after a clean boot**. After an abnormal reset, the first 32 kB go to `crash.log`, which is outside the rotation.
- Write `[boot] build reason boots crashes psram` at the top of every file.
- Add a read-only `/api/coredump` that streams the partition, and `/api/coredump/erase`.
- **Do not shorten FLUSH_MS (L2).** The flush becomes size-triggered (16 kB) or event-triggered (warning level or above, panic, explicit dump) instead of every 2 s. Routine lines stay in the RAM ring. Acceptance compares `[stall]` before and after.
- Optionally move the LogBus ring to PSRAM with `heap_caps_malloc(MALLOC_CAP_SPIRAM)` when `psramFound()`. This saves DRAM but does not avoid the cache stall.

**6e. Telemetry (every 10 s)**
- `[task]` per registered task, including logbus, cloud, ble_svc and ACAN2517Handler: stack low-water, wakes, busyMax.
- `[key]`: histogram, wire, pollGap, per-queue drops, stuckSynth.
- `[panel]`: foreignPolls, pollLateMax, mailbox posted/dropped, queueDropped, txGated, reinstalls, registered-after-RX time.
- `[car]`: fps, drainMax, decodeHits, ovf, fdDropped, maskMaxUs.
- `[hu]`: acks, ackLateMax, keyDropped, ackDropped, vehDeferred, replyRetries.
- `[log]`: LogBus dropped, file bytes.
- `[mem]`: internal and PSRAM free and minimum.
- `[stall]`.

**6f. t2can defaults (`platformio.ini`)**
- `lib_ignore = esp32_can, can_common` and `-D AFFA_ENABLE_CANCOMMON_LINK=0`, the `env:bench` pattern.
- `ARDUINO_EVENT_RUNNING_CORE=0`.
- Add `env:t2can_min`, which extends t2can and **keeps OTA**, as a bisect image.

**Files.** The ones above, plus `src/app/Supervisor.{h,cpp}`, `src/utils/CrashLog.{h,cpp}`, `src/utils/LogBus.{h,cpp}`, `src/utils/LogFile.{h,cpp}`, `src/net/CloudLink.{h,cpp}` (a pluggable health function) and `src/net/WebUi.{h,cpp}` (AppContext, coredump routes). Tests: `test_runtime` gains the supervisor predicate and the rotate-on-clean-boot decision.

**Risks**
- A WDT subscription on a task with a hidden long block causes reboot loops. That is why the trial day comes first and `[task] busyMax` is read before subscribing.
- The flash cache stall remains (L2). It becomes visible in `[stall]`, `[key] max` and `[car] ovf`, and the real fix needs a custom sdkconfig (Q6).
- The ROM boot output lands on the head unit's RX (GPIO43/44 = UART0 pins) on every reset, and the WDT adds resets. Count `[hu] junk` after reset and check how the head unit reacts.

**Acceptance**
- The standing check passes.
- An injected hang (`/api/diag/hang?task=keys`, compiled only with `-D DIAG_HANG=1`) reboots within 5 s with `reason=task_wdt`, and `/api/coredump` returns the dump.
- An injected hang during an OTA trial rolls back to the previous image.
- `crash.log` survives 10 forced reboots.
- `[log] dropped=0` under normal load.
- `pio run -e t2can -t size` plus `nm` shows no `CAN0` or `esp32_can` symbols.
- `git diff --stat` shows the net main.cpp reduction, and `tools/check_nav.py` passes.

---

## 5. Dead-code removal, folded into its step

These are removed **only in the step named**, after a grep shows no remaining caller. The list is the readers' findings. Anything not listed stays.

| Item | Step | Evidence |
|---|---|---|
| `g_panelKeyQ`, `drainPanelKeys`, `g_keyWaitWinMax` (replaced by `[key]`) | 2 | `main.cpp:369-392`, `:2547` |
| `Knobs::Service::_q` and `drain()` | 2 | `KnobService.cpp:84-102` |
| `g_macroLock` and its lock functions | 2 | `main.cpp:1217-1222` |
| `g_macros.tick()` in `scanTask` | 2 | `main.cpp:1488` |
| Stale comments: `main.cpp:362-366`, `:1219-1220`, `:2028-2034`; `KnobService.h:15-16` | 2 | the AMS and loop claims are false |
| `g_resyncRequest`, `g_clearRequest` flags | 3 | become mailbox ops |
| `delay(2)` at the end of `loop()` | 3 | replaced by the TWAI wait |
| Direct `twai_read_alerts` in `/api/can/alerts` | 3 | steals alerts |
| `canSelfTest`/`canLoopTest` in main.cpp | 3 | moved to `diag/CanBench` |
| `src/bus/Mcp2518Bus.{h,cpp}` | 5 | replaced by `Mcp2518Source` |
| `busRx` `#if` source blocks in `loop()` (`main.cpp:2610-2615`, `:2066-2069`) | 5 | car task source list |
| `WebUi::_setRole` member and its attach argument | 6 | never called (`WebUi.h:181,355`). **`applyCanRole` is NOT dead**: it is called at boot (`main.cpp:1703`) |
| ~40 `WebUi::attach*` raw-pointer setters | 6 | replaced by AppContext |
| `esp32_can`/`can_common` linked on t2can | 6 | `platformio.ini:164-165`, `lib_ignore` |
| `s_tapOwner` | 6 | replaced by the TLS guard |

Never removed: anything under `src/display/`, the `FEATURE_USB_HID` blocks, and the MeganeCAN BLE files.

### A1-A8 mapping (from PLAN-nonblocking-callbacks.md)

| Item | Where it goes |
|---|---|
| A1 | AMS can no longer block keys (Step 2, coalesced on ble_svc). The write-with-response itself stays (Q3) |
| A2 | Step 2 |
| A3 | Step 2 (§3.4) |
| A4 | Steps 1, 3 and 6 |
| A5 | Half done in Step 2 (EmitFn). The scanner split is not needed |
| A6 | Step 2 (plain code in KeyMsg) |
| A7 | Steps 3-4 via our mailbox. `AFFA_ENABLE_TASK` stays 0 |
| A8 | Kept (HID on the keys task) |

---

## 6. Production checklist (T-2CAN in the car)

- [ ] Every key path task is pinned (§2.1) and appears in `[task]`.
- [ ] `[key] p99 < 3 ms` source-to-write. `wireMax` is reported. 0 keyDropped, 0 stuckSynth in normal use.
- [ ] `[hu] ackLateMax(wire) < 8 ms`, 0 ackDropped.
- [ ] `[panel] foreignPolls=0`. The owner assert is clean. TX gating works with the panel unplugged.
- [ ] `[car] ovf=0` at bus rate and during a log rotate. `maskMaxUs` is measured and the core decided.
- [ ] TWDT covers keys, hu, panel, car, ui and knobs. An injected hang leads to a coredump and a rollback during a trial.
- [ ] The OTA verdict is liveness-based and peripherals are neutral.
- [ ] `crash.log` survives a boot loop. `/api/coredump` works offline.
- [ ] IDF logs reach the flash log. No `g_ws.printf` remains on hot-path tasks (`check_rules.py`).
- [ ] The build id is in `[boot]`, `/api/health` and the canbox version message.
- [ ] `esp32_can` is not linked. `env:t2can_min` exists and has OTA.
- [ ] `env:t2can_usbhid` compiles.
- [ ] The emitter stays off until the decode table is car-verified (owner switch).
- [ ] Head-unit reaction to the ROM boot output after a reset is checked.

---

## 7. Appendix: critique items decided

### Rejected or changed

- **"INT=255 removes the library task"** (draft). This is false, as all three critics found (`ACAN2517FD.cpp:505`). Replaced by our own ISR with INT=8.
- **"Wrap `receive()` in `portENTER_CRITICAL`"** (draft fallback). `beginTransaction` takes a mutex inside the critical section, which asserts. Dropped.
- **Concurrency critic B1: `vTaskDelete(xTaskGetHandle(...))` as the primary fix.** It works, but `end()` would then delete a dangling handle (:559-562). It is kept as an optional RAM recovery; our own ISR is the primary fix.
- **Seqlock<T>** (draft). Replaced by portMUX snapshots. The structs are small, and a seqlock reader can livelock on a unicore C3 (migration m4, concurrency 12).
- **Arduino UART event task plus hu task** (draft). One hu task with a 2 ms poll is enough at 38400 baud, and it avoids global serial-event flags (migration m4).
- **`/api/canbox/raw` stays synchronous through `writeKey`** (migration m1). Rejected: httpd moves to core 0, and a core-0 mutex holder can be preempted by radio tasks (concurrency 3). It posts to `UartOut` instead.
- **Moving AMS and HID both to ble_svc** (draft). HID is kept on the keys task (concurrency 6, latency M3). Only AMS goes to ble_svc, and it is coalesced.
- **`VEH_INFLIGHT_MAX = 48 B`** (draft). That is 12.5 ms, which is over the 10 ms ack window (latency B2). Replaced by the empty-FIFO gate, not mid-RX-frame, with `L_max` measured.
- **Knobs at priority 4** (draft). Raised to 6 (latency M1, migration M3).
- **Step 6 `panic=false` trial** (draft). It would downgrade the existing IDLE0 panic (latency m7). Replaced by a log-only Supervisor.
- **Shorten FLUSH_MS to 1 s** (draft). This makes flash stalls worse (latency B1). Replaced by a size- or event-triggered flush.
- **The verdict requires panel, car fps and HU RX** (draft). This would roll back good images with ACC off (latency M7). Those checks are now neutral.
- **Reboot when ble_svc is stale** (draft). Rejected: it costs keys and panel for a BLE fault (latency M7, concurrency 19).
- **"applyCanRole is dead"** (draft). It is called at boot (`main.cpp:1703`). Only `_setRole` is dead (migration M1).
- **Sources chosen at build time** (draft). The DevKit can run Link and TWAI vehicle together, so the sources are a list chosen at boot (migration M1).
- **The NVS/JSON decode editor in Step 4** (draft). Deferred; the compiled table is enough (migration m4).
- **Seven steps (0-6)** (migration m8). Folded into six to match the request.
- **Panel idle wait of 10 ms** (draft). Reduced to 2 ms, with 1 tick while the mailbox is non-empty (concurrency 17, latency m1).
- **Keys-task timeout "≤10 ms while pending"** (draft). Replaced by the exact next deadline (latency m9).
- **httpd lowered only in Step 6** (draft). Moved to Step 1 (concurrency 10).
- **Align the native AffaDisplay pin in Step 6** (draft). Moved to Step 1 (migration M5).
- **OTA liveness only in Step 6** (draft). The registry-based liveness goes in Step 1 (migration M6).

### Accepted with no change in intent

Concurrency 4, 5, 7, 8, 9, 11, 13, 14, 15, 16, 18, 20 and 21.

Latency M2, M4, M5, M6, M8, m2 (the accept-all filter: per-wake drain plus a note; the hardware filter is left for when CAN B is shared), m3, m4, m5, m6, m8 and m10.

Migration M2, M4, m2, m3, m6 and m7. m7 is resolved by listing the removals in §5.

---

## Open questions for the owner

- **Q1.** Are the six steps here the "six steps" you meant? Step 1 is scaffolding plus cheap fixes, and Step 6 is hardening plus the main.cpp split.
- **Q2.** `BleHub.cpp` and `HidRole.cpp` carry MegaOpen feature gates, so they look like MegaOpen code rather than the MeganeCAN copy. May Step 2 add the one-line `Hid::externallyServiced()` skip in `BleHub::Service`, so that HID press and release run on the keys task?
- **Q3.** AMS write-with-response (A1) and the `Detach()`-vs-write race in `apple_media_service.cpp` are MeganeCAN code. Leave them, or allow a minimal fix (no-response writes, a null check under a lock)?
- **Q4.** `/ws` (WsStream) still exists as a LogBus sink. Keep it as an optional sink, or remove it in Step 6?
- **Q5.** `env:bare` and `env:boardv` have no OTA, against the rule. Add OTA, or document them as USB-only bring-up images?
- **Q6.** Flash erases freeze both cores (L2). Is a custom sdkconfig/IDF rebuild with `SPIRAM_FETCH_INSTRUCTIONS/RODATA` acceptable later, or do we live with event-only flushes?
- **Q7.** Is a GPIO free on the T-2CAN for ACC/ignition sense, so logs can be flushed before power loss?

## Decisions taken for implementation (2026-10-09)

The owner approved all six steps. The open questions are settled conservatively here, so the work is not blocked. Any of them can be revisited:

- **Q1:** yes. These are the six steps.
- **Q2: no BleHub change.** HID takes the plan's fallback and runs on ble_svc. HID items are handled before AMS, and `Hid::service()` is also called between AMS writes. UART is the main sink and HID is not in use, so the latency cost lands on an idle path. The "MeganeCAN byte for byte" rule is kept whole.
- **Q3: leave** `apple_media_service.cpp` untouched. AMS can no longer block keys anyway, because it is coalesced on ble_svc.
- **Q4: keep `/ws`** as an optional LogBus sink. Nothing on a hot-path task writes to it.
- **Q5:** no OTA is added to `bare`/`boardv` in this branch. They are documented as USB bring-up images.
- **Q6: deferred.** We use event- and size-triggered flushes only. There is no custom IDF build.
- **Q7: deferred.** There is no ACC sense.
- **Hardware:** nothing is flashed to the bench DevKit by the implementing agents. Bench and car acceptance (the `[key]`, `[panel]`, `[car]` and `[hu]` figures) is left to the owner. Every step must pass every env build and `pio test -e native` before it is committed.

## Step 5b: CAN capture and mapping tooling (owner, 2026-10-09)

The head unit's receive side is now decoded (`docs/hu-decoder.md`). The main work from here is finding each signal on the car's bus and sending it over Raise: fuel level, range, coolant, PRND and more. That means long captures recorded in the car and analysed on the PC afterwards. This step builds on Step 5's car task and is part of it.

**Owner decisions**
- T-2CAN flash is re-split:
  - app0 and app1: 3 MB each (0x300000), at 0x10000 and 0x310000;
  - littlefs: 0x610000, size 0x9E0000 (~9.9 MB);
  - coredump: unchanged at 0xFF0000.
- Captures are stored in a compact binary format. A PC converter turns them into SavvyCAN CSV and candump text.

**On the board**
- **Recorder** (`src/capture/CanRecorder.{h,cpp}`, with the record format portable and tested):
  - The car task, and optionally the panel bus, push frames into a PSRAM ring (1 MB when `psramFound()`, 32 kB otherwise). A `capwr` task (priority 1, core 0) writes 16 kB chunks to `/cap/NNNN.bin`. When the files reach 90 % of the filesystem, the oldest is deleted.
  - Recording never blocks a producer. A full ring drops frames and counts them.
- **Format.**
  - File header: magic `MOCAP1`, the build id, the boot number, the bitrate of each bus, and the wall clock if it is known.
  - Each record is fixed-size: `u32 t_ms`, `u32 id | ext<<31 | bus<<29`, `u8 len`, `u8 kind (frame/marker/stats)`, `u8 data[8]`.
  - **Change-only mode is the default.** An id is written when its data changes, plus a keyframe for every id every 5 s. A per-id repeat count goes into a stats record every second, so frame rates stay recoverable.
  - A full mode writes every frame, for short bursts.
- **Markers.**
  - `/api/cap/mark?t=fuel+3/4` writes a marker record. A panel key combo (configurable) writes a numbered marker while driving.
  - Markers also go into the LogBus log.
- **Control.**
  - `/api/cap` covers start, stop, mode, status, list, download (streamed), and delete.
  - An "autostart on boot" setting means the owner only has to drive.
  - The heartbeat gets a `[cap]` line: frames, written, dropped, file and free space.
- **Decode table hot-load.** `/decode.json` on littlefs overrides the compiled `decode_defaults.inc`. It is uploaded from the web and handed to the car task as a `Staged<T>`, with no reboot. This turns "try a byte guess" from an OTA (~1 min) into an upload (seconds). It replaces the earlier "deferred" decision on the JSON override.

**On the PC** (`tools/can_dump.py`, Python stdlib only):
- `convert`: binary to SavvyCAN CSV (GVRET) and candump.
- `ids`: per-id rate, length and byte-change counts.
- `between A B`: which bytes changed between two markers.
- `slow`: bytes that move slowly and monotonically, which is how fuel and odometer look.
- `track ID byte`: one byte's value over time, for plotting.

**Acceptance (bench).**
- Native tests round-trip the record format and the change-only logic.
- The DevKit builds with a small RAM ring.
- On T-2CAN, a 1-hour drive fits.
- `[cap] dropped=0` at bus rate.
- `[stall]` and `[key] p99` are compared with the recorder on and off.

The partition change needs a USB flash once, because OTA cannot change the partition table. Note it in the commit and in the t2can wiring page.

### 5b additions (owner, same day): self-managing storage, and the mapping page

**Storage manages itself.** The default is a ring, and nothing has to be configured:
- **Sessions.** Each boot or start opens a new session file, `/cap/NNNN.bin`. A file closes at 4 MB and the next one opens.
- **Ring.** When the files reach 85 % of littlefs, the oldest *unprotected* session is deleted.
- **Protection.**
  - A session that contains a marker is protected automatically.
  - A session is also protected when the owner presses "keep".
  - Protected files are deleted only when nothing else is left, and the log says so.
- **Cleanup options** on the page and as `/api/cap/...`:
  - delete one file;
  - delete everything unprotected;
  - delete everything older than N days or sessions;
  - unprotect;
  - format the capture area (with a confirmation).
- **Retention settings** (defaults in brackets): ring on/off [on], max fill % [85], file size [4 MB], auto-protect sessions with markers [on], change-only or full [change-only], autostart [on].
- **Flash use.** The application log (`/log/*.log`) and the captures share littlefs. The log keeps its own fixed cap of 2 MB, so a capture can never push the log out, and the reverse is also true.

**The bus configures itself.**
- At start, the car channel runs in **listen-only** mode and tries 500k, then 250k, then 125k, then 1M. It keeps the first rate that gives frames with no error frames, and saves it in NVS.
- A saved rate is tried first on the next boot. A rate set by hand in the settings always wins.
- Listen-only stays on until the owner switches the channel to normal. The car bus is never ACKed or transmitted on by default.
- `[cap]` and `/api/cap` report the detected rate and the frames per second.

**The mapping page** (`/map`, generated by gen_pages.py like the other pages):
- **One row per Raise/Toyota function** the head unit decodes, taken from `docs/hu-decoder.md`:
  - speed, rpm, odometer and trips;
  - the 0x7D 04 range/fuel field;
  - coolant (0x7D 09), outside temperature (0x28 D5);
  - doors and bonnet (0x24 len 2 and 0x7D 05);
  - lights, indicators and fogs (0x7D 01);
  - PRND (0x7D 06);
  - steering (0x29 / 0x7D 08);
  - reverse/radar;
  - and the rest of §2.
  - Each row says what the head unit shows for it.
- **For each row the owner binds a Megane source:**
  - CAN id;
  - start byte and bit, length, endianness, signed or unsigned;
  - scale, offset and clamp;
  - or "constant" or "off".
  - The ids seen on the bus are offered in a list (from the recorder's id table).
- **Live view.** Each row shows the bound bytes as raw hex, the decoded value, and the exact Raise frame that would go out. These come from a snapshot the car task publishes about 4 times a second; httpd never reads the bus.
- **Helpers:**
  - "learn": press, then change the thing in the car, and the page lists the bytes that changed (the same logic as `can_dump.py between`);
  - "test send": send this row's Raise frame once with a typed value, to check the head unit's icon without the car;
  - per-row enable, so one bad guess cannot spam the head unit.
- **Save** writes `/decode.json`, which the car task applies at once (Staged, no reboot). Export and import of the JSON let the owner share one Megane's map with friends. The compiled `decode_defaults.inc` remains the factory reset.
- The rows come from one table in code (`src/vehicle/RaiseTargets.{h,cpp}`): the Raise function, its Sig, its units and range, and its builder in `Rav4Emitter`. **Adding a new Toyota function means adding one row there.** The page and the JSON schema follow from that table automatically.
