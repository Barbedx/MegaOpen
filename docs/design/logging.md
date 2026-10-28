# Logging: one model, configurable, cheap

Status: **plan**, 2026-10-28, owner's request ("structure and architecture of the
logs as a task of its own; configurable, sensible, optimised"). Nothing here is
built yet; the slices at the end are the order.

## What there is today (measured on master 64173c5)

| piece | what it does | the problem |
| --- | --- | --- |
| `Log::emit` / `LOGE..LOGT` | level + `[cat]`, gated by one global level | used by **77** calls only |
| `LogBus::printf` / `line`, `logf` | straight into the bus | **258** calls, never gated by level; the level is half a feature |
| level setting `logLevel` | ERR WRN INF DBG TRC | volatile: every boot is INF again |
| sinks: web ring (PSRAM 32 kB), flash (`/log`, 8 x 256 kB), push (PSRAM 1 MB spool), Serial | fan-out of every line | each sink filters by **matching text**: `isWebOnly` (`[kev]`, `[hur]`), `isHuEcho`, `serialOnly`, the push's `huChanged` dedup |
| periodic lines every 10 s | `[hb] [win] [task] [stk] [can] [panel] [stall] [loop] [hu] [key] [car] [can2] [cap] [ble] [media] [ui] [drive] [gear]`... | ~20 lines / 10 s: most of the 1 MB/h a drive pushes is the same counters again |
| head-unit UART | `[hu] T/V/R/J` echo of every frame (always on), `[hur]` decoded | the flood that crash-looped the car (1.6.0-rc5); `J junk FF` is the head unit's ACK, not junk |
| CAN | the capture recorder (binary, change-only) | good; the model the UART should follow |

## The model

1. **Every line has a level and a tag, and nothing else decides whether it is written.**
   One API, `LOG(level, tag, fmt, ...)` (the `LOGx` macros). The level is checked
   *before* formatting, so a DBG line costs nothing at INF. `LogBus::printf` and
   `logf` stay internal to the logger. No sink ever looks at the text of a line.

2. **Tags are a fixed list, not strings found by prefix.** One table
   (`Log::TAGS`: `boot hb wifi push ota car can2 panel hu key knobs ble diag cap
   alert ...`), each with its group (KEY PNL BLE HU CAN CAP NET SYS, as the web
   log's filter already has). A tag id is a byte: cheap to carry and to filter.

3. **Levels** (as now): `ERR` failure · `WRN` handled but wrong · `INF` state
   changes (boot, joined WiFi, alert fired, OTA, crash) · `DBG` per operation
   (a key press, a detent, a frame from the head unit, a push) · `TRC` (verbose)
   every frame, every byte.

4. **Configuration, one card «Журнал», saved in NVS:**
   - the level, from a list: ERR / WRN / INF / DBG / TRC;
   - per-tag overrides, from a list next to each tag ("hu: TRC", rest INF), for
     looking at one thing without the firehose;
   - per sink, the lowest level it takes: web ring, flash, GitHub, USB serial
     (defaults: all = the chosen level; flash at most DBG, it erases sectors).
   The panel menu gets the level only (one row).

5. **Status is not a line per module.** Each module registers a status provider
   (a struct it fills: counters, gauges). The logger writes:
   - at INF, one combined `[status]` line a minute, only fields that changed;
   - at DBG, every 10 s (today's cadence);
   - to GitHub, a JSON snapshot per boot and every 10 minutes
     (`boot-N/status-HHMM.json`): machine-readable for tools, not regexes.
   The interval is a setting.

6. **High-rate data is never text.** CAN frames already go to the capture file.
   The head-unit UART goes there too, as bus 2 of the same format (`CapFormat`
   has a bus byte): every frame both ways, timestamped, change-only like CAN.
   That replaces the `[hu]` echo, `[hur]`, `serialOnly` and the push's dedup.
   At TRC a frame may still print, for a live look.
   The head unit's `0xFF` ACKs are counted (`frames out / ACKs in / missing`),
   not logged as junk.

7. **The logger protects the board.** Built in, not per caller:
   - repeats fold: "last line x N" instead of N lines;
   - a per-tag budget at DBG/TRC (lines per second), the excess counted
     (`[log] hu: 340 lines dropped`);
   - the push's TLS pacing stays (a backlog every 6 s, IDLE priority).

8. **One format everywhere:** `<ms> <L> [tag] message`, values as `key=value`
   where a tool will read them. `tools/logscan.py` filters a pushed boot by
   tag and level and joins the status snapshots into a table.

## What it costs and saves

- Formatting only for lines that will be written (today 258 calls format at
  any level).
- A drive's pushed text log: today ~1 MB/h, mostly repeated counters; with
  status a minute and the UART in the capture, an estimate of ~100 kB/h at INF.
- The UART history becomes complete (every frame), where today it is either a
  flood or deduplicated.

## Design decisions (checked against the code, 2026-10-28)

**The line keeps its text format, and the format is the metadata.** Every line
the logger writes is `<L> [tag] message` (L = E W I D T), as `Log::emit` already
writes; `LogLine::parse` (LogRing.cpp) reads level and tag from the first bytes
without scanning the message. So the ring (`LogRing`, records `[len][ms][text]`)
does not change, and a sink that needs the level parses two characters, not a
pattern. A legacy line with no `<L>` counts as INF until slice 2 removes them.

**Tags register themselves.** No hand-kept list to drift: the macro carries a
per-call-site cache,
```
#define LOGD(tag, ...) do { static int8_t s_t = -1;                     \
        if (Log::on(LogLevel::DBG, tag, s_t)) Log::emit(LogLevel::DBG, tag, __VA_ARGS__); } while (0)
```
`Log::on` finds or adds `tag` in a 64-entry table once (s_t then holds its
index), and from then on is one array read and a compare: the tag's override
level if one is set, else the global level. `/api/logs/tags` lists the tags
seen this boot, for the page's per-tag list. `Log::TAG_ROWS` stays only as the
web filter's grouping (tag -> KEY PNL BLE HU CAN CAP NET SYS).

**Settings** (registry, NVS, applied at once; group «Журнал»):

| key | type | default | meaning |
| --- | --- | --- | --- |
| `logLevel` | enum ERR..TRC | INF | global threshold; **persisted** (today volatile) |
| `logTags` | text `tag=LVL ...` | empty | per-tag thresholds, e.g. `hu=TRC knobs=DBG wifi=WRN`; the page edits it as a list of rows (tag ▾, level ▾) |
| `logWeb` `logFlash` `logPush` `logSerial` | enum ERR..TRC | TRC, DBG, TRC, INF | the lowest level each sink takes; a line goes to a sink when it passed the threshold AND its level <= the sink's |
| `logStatusS` | enum 10 / 60 / 300 s | 60 | the `[status]` line's interval at INF (DBG: 10 s) |

TRC on the flash sink is refused (it erases sectors with both cores stopped).
The web sink filters at read time (`/api/logs`), so a raised web level shows
lines that are still in the ring.

**Sinks** become: Serial, flash, push, each `LogBus::addSink(fn, Log::SINK_x)`;
`LogBus::fanOut` skips a sink whose level the line exceeds. The four text
filters (`isWebOnly`, `isHuEcho`, `serialOnly`, `huChanged`) are deleted in
slice 4, when nothing needs them.

**Status providers** (slice 3): `Log::addStatus(tag, fn)` where `fn(char* out,
size_t cap)` writes `key=value ...`; the logger calls each at the interval,
compares with what it wrote last, and puts the changed ones on one
`I [status] tag: k=v k=v | tag: ...` line (folded under MAX_LINE, continued on
a second line if needed). The JSON snapshot to GitHub calls the same providers.
Today's `[hb] [win] [task] [stk] [can] [panel] [stall] [loop] [hu] [key] [car]
[can2] [cap] [ble] [media] [ui] [drive] [gear] [push]` periodic lines each
become one provider; their tasks no longer print on a timer.

**As built (step 3, 2026-10-28):** each task keeps writing its own status
line from its own counters (only it owns them; a provider called from the
logger's task would read them across tasks), through `LOGS(tag, ...)`, which asks
`Log::statusOn(tag)`: due every window at DBG, else once per `logStatus`
interval. A task with several lines per window (`[task]`) asks once and writes
them all. The combined `[status]` line stays to do.

**As built (status JSON, 2026-10-28):** no providers for it either. The
snapshot is what the page reads: `GET /api/status` and `GET /api/health` in
one object (`WebUi::statusJson` / `healthJson`, the routes' own builders),
`{uptimeMs, status, health}`, sent by LogPush as
`boot-N/status-MMMM.json` (uptime minutes) right after the boot's first push
and then every 10 min (`LogPush::SNAP_MS`). Its JSON pools are in PSRAM
(`utils/PsramJson.h`), beside the TLS job.

**What each level carries after slice 2** (the rules for converting a call):

| level | what goes there | examples |
| --- | --- | --- |
| ERR | something failed and was not recovered | littlefs mount failed, TLS refused the token |
| WRN | handled, but wrong | bus fault, push retry, crash summary, alert dropped |
| INF | a state changed, once | boot, WiFi joined/lost, OTA phase, alert fired, push of a file, setting saved |
| DBG | one operation | a key / detent / click, a head-unit command, a diag request and reply, a capture file closed |
| TRC | every frame or byte | panel frames, UART frames, CAN ids, the [hu] text column |

Repeats fold in `Log::emit`: the same tag and message within 2 s becomes one
`... (x N)` line when it changes. The per-tag budget is 20 lines/s at DBG and
200/s at TRC; over it, lines are counted, and `W [log] hu: N lines over budget`
is written once a second while it lasts.

**UART in the capture** (slice 4): `CanRecorder::push` takes a frame with a bus
id (CAR 0, PANEL 1 today); the head-unit link adds **HU 2**, one record per
frame, `id` = the command byte, direction in the id's top bit (0x100 = from
the head unit), data = the payload. Change-only applies per (direction, cmd,
sub). `tools/can_dump.py` prints bus 2 as `HU> 7D 03 ...` / `HU< 83 30 05`.
The ACK `0xFF` from the head unit increments `hu acks_in`, matched to
`frames_out`; a missing ACK in 100 ms counts `acks_missing`.

**Tests** (env:native): the tag cache and overrides, the per-sink routing, the
folding and the budget in `test_log`; `test_logring` unchanged; a check in
`tools/check_rules.py` that no file outside `src/utils/Log*` calls
`LogBus::printf` / `line` after slice 2.

**Risks:** a mechanical conversion of 258 calls can change a line a tool or a
test greps (`[ota] CONFIRMED`, `[boot] reason=`, `rc_smoke`); those keep their
text exactly and gain only the `I ` prefix, and the tools accept both.

## Slices

1. **Core:** the tag table, `LOG()` with per-tag and per-sink levels, levels
   saved in NVS; the web log's filter uses the tag ids. Callers untouched.
2. **Callers:** every `LogBus::printf("[x] ...")` / `logf` becomes `LOG?(x, ...)`
   at its proper level (mechanical; `W [x]` and `E [x]` prefixes become WRN/ERR).
   `tools/check_rules.py` then forbids `LogBus::printf` outside the logger.
3. **Status:** providers for the ~20 periodic lines, the combined line, the
   JSON snapshots on GitHub, the interval setting.
4. **UART into the capture:** bus 2, ACK counting; the `[hu]` echo, `[hur]`
   line, `isWebOnly` / `isHuEcho` / `serialOnly` / `huChanged` deleted.
5. **Page:** the «Журнал» card (level, per-tag, per-sink, status interval); the
   panel menu's level row.
6. **Tools:** `tools/logscan.py`; `tools/cap_fetch.py` / `can_dump.py` read bus 2.

Each slice builds, passes the host tests and runs on the bench before the next.
