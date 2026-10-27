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
