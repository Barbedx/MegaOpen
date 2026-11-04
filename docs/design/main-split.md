# main.cpp split (architecture item 1, 2026-11-04)

The owner's ask: main.cpp holds work that belongs to the task modules; move
each piece next to its owner, leaving `setup()` as the wiring list. This is
the code-motion half of `event-tasks.md` Step 6a, done by owner rather than
into one `App.cpp` (which would only rename the glue file).

## Rules for the move

- **Code motion only.** Same code, same `#if` gates, same order of
  construction in `setup()`, same log lines (text and tags). Nothing is fixed
  on the way: what is seen goes to `docs/BACKLOG.md` «Findings».
- **One owner per object.** A module keeps its objects file-static and hands
  out what others need through accessors (the house style of `App::Panel`,
  `App::Ui`, `Mcp2518Source::instance()`). `Cfg::Config`, `Vehicle::State`,
  `WebUi` and `WifiManager` stay in main.cpp, passed to `begin()`.
- **No static constructor reads another file's object** (the cross-file
  init order is undefined). Checked per slice: every moved global's
  constructor stores only its own fields.
- `tools/check_rules.py` follows the functions to their new files (the
  keys/hu/car task function lists, `RAW_LINE_ALLOWED`, the panel owners).

## Where each piece goes

| piece in main.cpp today | new home |
| --- | --- |
| boot reason and counter, crash summary, coredump keep, `memMark` | `src/app/BootLog.{h,cpp}` |
| LogBus sinks (serial, flash, push), the shutdown flush, the IDF tap, the log settings at boot | `src/app/LogSetup.{h,cpp}` |
| Serial1, `UartTx`, the protocol's emitter and decoder, the frame ring and its echo, `/api/canbox/last`'s JSON, junk batching, the `onHu*` callbacks | `src/app/HuLink.{h,cpp}` (next to `HuTask`, which stays the generic engine) |
| `performAction`, `macroRows` | `src/app/RuleActions.{h,cpp}` |
| the key objects (engine, matrix map and scanner, UART/USB/ladder sinks, BLE key proxies), the staged web edits, the gesture engine and filter, `onKeyEvent`, the keys task's hooks, the scan task, the matrix and ladder GPIO | `src/app/KeyPath.{h,cpp}` (next to `KeysTask`) |
| the panel's keys: `AffaKeySource`/`AffaMap`, `onPanelKey`, `onAffaKeyReport`, `formatKeyLine`, `publishKeyLast` | `src/app/PanelKeys.{h,cpp}` |
| now-playing: `MediaRouter`, `HuMediaSource`, `onMediaChange`, their boot | `src/app/Media.{h,cpp}` |
| `bleServiceTask`, the ANCS callback, the BLE boot block | `src/ble/BleService.{h,cpp}` |
| the TWAI link, the raw frame ring, `carTap`, `carLinkPump`, the loop pass and its `[can]`/`[stall]`/`[loop]` window, `loopWait` | `src/app/LoopTask.{h,cpp}` |
| the display's construction, `affaTap` and the frame trace, `panelWatchStep` | `src/app/PanelOwner.cpp` |
| `hkWeb`, `hkWifi`, `hkWifiSeen`, `addressText`, `logPushSnapshot` | `src/app/NetHooks.{h,cpp}` |
| `hkBeat`, `hkBeatSlow` (`[stk]`, `[can2]`, `[ble]`, `[media]`, `[hb]`, `[net]`) | `src/app/Telemetry.{h,cpp}` |

What stays in main.cpp: the four shared objects, `setup()` as an ordered list
of `begin()` calls, the knob and car-task wiring, the `WebUi` attach list
(item 2 replaces it), and `loop()` = `App::Loop::pass()`. Expected size after:
a few hundred lines; under 150 once item 2 hands WebUi one context.

## Slices

Each slice is one commit: builds `t2can`, `t2can_usbhid`, `direct`; host
tests; `check_rules`, `check_i18n`, `check_pages`.

1. BootLog + LogSetup.
2. HuLink.
3. RuleActions.
4. KeyPath + PanelKeys.
5. Media + BleService.
6. LoopTask + the panel's glue into PanelOwner.
7. NetHooks + Telemetry.
8. main.cpp tidy; ARCHITECTURE.md source map, `event-tasks.md` Step 6a,
   CLAUDE.md's symptom table.

Then on the bench (OTA, `[ota] CONFIRMED`, `tools/rc_smoke.sh`): the boot log
has the same lines as 1.9.0's, `[mem]` marks and idle `[hb]` heap within a
kB, `[key]`/`[hu]`/`[panel]` lines unchanged.
