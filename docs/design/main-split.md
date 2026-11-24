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

## Where each piece went

Done 2026-11-04 (8 commits, `main split 1..8`). main.cpp: 2867 -> ~480 lines.

| piece of main.cpp | home |
| --- | --- |
| boot reason and counter, crash summary, coredump keep, `memMark` | `src/app/BootLog.{h,cpp}` (`App::Boot`) |
| LogBus sinks (serial, flash, push), the shutdown flush, the IDF tap, the saved log levels | `src/app/LogSetup.{h,cpp}` |
| Serial1, `UartTx`, the protocol's emitter and decoder, the frame ring and its echo, `/api/canbox/last`'s JSON, junk batching, the `onHu*` callbacks | `src/app/HuLink.{h,cpp}` (next to `HuTask`, which stays the generic engine) |
| now-playing: `MediaRouter`, `HuMediaSource`, their boot | `src/app/Media.{h,cpp}` |
| `performAction` (now `App::RuleActions::perform`), `macroRows` | `src/app/RuleActions.{h,cpp}` |
| `PanelOps`, the object every render goes through | `App::Panel::ops()` (`src/app/PanelOwner.*`) |
| every key object (engine, matrix map and scanner, UART/USB/ladder sinks, BLE key proxies), the staged web edits, the gesture engine and filter, `onKeyEvent`, the keys task's hooks, the scan task, the matrix and ladder GPIO, the panel's keys (`AffaKeySource`/`AffaMap`, `formatKeyLine`, `publishKeyLast`) | `src/app/KeyPath.{h,cpp}` (next to `KeysTask`) |
| `bleServiceTask`, the ANCS callback, the BLE boot block | `src/ble/BleService.{h,cpp}` |
| the TWAI link, the raw frame ring, `carTap`, `carLinkPump`, the display's construction, `affaTap` and the frame trace, `panelWatchStep`, the loop pass and its `[can]`/`[stall]`/`[loop]` window, `loopWait` | `src/app/LoopTask.{h,cpp}` (`App::Loop`; the panel task's file, beside `PanelOwner`) |
| net_svc's hooks: `hkWeb`, `hkWifi`, `hkWifiSeen`, `addressText`, `logPushSnapshot`, `hkBeat`, `hkBeatSlow` | `src/app/NetHooks.{h,cpp}` |

Changed from the plan: the panel's keys joined KeyPath (they run on the keys
task and need its sinks, so a PanelKeys file only added accessors); the
panel's glue went to LoopTask, not PanelOwner (the loop owns the TWAI the
panel shares with env:direct's car bus); the heartbeat lines stayed with the
other net_svc hooks (one file, `NetHooks`) rather than a Telemetry file.

What stays in main.cpp: the shared objects (`Cfg::Config`, `Vehicle::State`,
`WebUi`, `WifiManager`, the sniffer, the knobs), `setup()` as the ordered
list of `begin()` calls, the knob and car-task wiring, the `WebUi` attach
list (item 2 replaces it), and `loop()` = `App::Loop::pass()`.

## Slices

Each slice is one commit: builds `t2can`, `t2can_usbhid`, `direct`; host
tests; `check_rules`, `check_i18n`, `check_pages`.

1. BootLog + LogSetup.
2. HuLink + Media; check_rules looks a glue function up in every glue file.
3. RuleActions; PanelOps to the panel owner.
4. KeyPath (with the panel's keys).
5. BleService.
6. LoopTask (with the panel's glue).
7. NetHooks.
8. main.cpp tidy; the comments that named main.cpp; ARCHITECTURE.md,
   `event-tasks.md` Step 6a, CLAUDE.md's symptom table.

Then on the bench (OTA, `[ota] CONFIRMED`, `tools/rc_smoke.sh`): the boot log
has the same lines as 1.9.0's, `[mem]` marks and idle `[hb]` heap within a
kB, `[key]`/`[hu]`/`[panel]` lines unchanged.
