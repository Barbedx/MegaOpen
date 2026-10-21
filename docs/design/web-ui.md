# Design: web UI v2

> Design notes kept from development: the plan, its steps and the decisions
> taken on the way. Where they disagree with the code, the code is right.
>
> **Since then:** `/api/canbox/tuning`, `/api/cap/settings`, `/api/routing`,
> `/api/loglevel` and `/api/messages/enable` are folded into `/api/settings`
> (the settings registry, `src/cfg/SettingsRegistry.cpp`); the panel alerts
> moved from `/api/map` to `/api/settings` `alerts`. `/api/link*`, the DevKit
> + board V setup and `docs/bench.md` with its bench CAN instruments are
> deleted.

Branch `feat/ui-v2`, cut from `refactor/event-tasks` at 2e04085. Written 2026-10-16.

The source of truth for what the screens look like is the v2 simulator (`py tools/build_sim.py`, output in `build/`). The owner reviewed it screen by screen and approved it on 2026-10-16.

## Owner rules

These come on top of the rules in [event-tasks.md](event-tasks.md).

1. **Simplify, do not pile on.**
   - Every slice reports net lines and the route count, before and after.
   - A new route exists only if it replaces older ones; the slice names what it replaces.
   - Delete before adding.
2. **Minimal hard-coding in the UI.**
   - Layouts, styles, pane scenes, panel types, BLE roles, actions, key codes, panel keys, console commands and Raise fields all grow over time.
   - The firmware owns each of those lists, once, and serves them through `GET /api/catalog`.
   - The page renders whatever the catalog says. Adding a style means one table row in the firmware, and no UI edit.
3. **Split roles and review every slice.** See "Roles" below.
4. **No WebSocket.** The live log is read with `GET /api/logs?since=N`.
5. **No flashing without the owner's word.** COM4 is never opened. OTA happens only on request.

## Roles

| Role | Who | Owns |
|---|---|---|
| Lead | main session | This plan and the API contracts. Writes each slice spec, integrates, runs the builds, commits, keeps the line and route ledger. |
| Implementer | one sub-agent per slice | Writes the slice to its spec. Does not change scope. Anything else it finds goes into a "found" list, not into the diff. |
| Reviewer | a fresh sub-agent per slice that has not seen the implementation | Adversarial, read-only. Checks against: the spec; the task-ownership rules (httpd only posts, stages or reads snapshots); heap and stack on httpd; error paths returning the right status; dead code left behind; hard-coded lists in the page; net lines. |
| Owner | Andrii | Accepts on the board and in the car. |

**Order within a slice:**
1. Lead writes the spec.
2. Implementer does the slice.
3. Lead builds `t2can`, `t2can_usbhid`, `devkit` and `direct`, and runs `pio test -e native`.
4. Reviewer reviews.
5. Implementer, or the lead, fixes the confirmed findings.
6. Lead rebuilds and commits.

There is one commit per slice, and the commit message carries the ledger line.

## Source layout

The page stops being nine R"PAGE( )PAGE" strings. It becomes one app with modules:

```
web/
  index.html        shell: header, nav, main, footer
  app.css
  js/               concatenated in file-name order (00-*.js first)
    00-util.js      $, esc, formatting
    10-api.js       call(): fetch, error -> {status, message}
    20-kit.js       row/card/sel/seg/sw, saver (autosave), reboot bar, ask()
    30-glass.js     the panel preview, driven by the catalog's layout geometry
    40-home.js … 90-service.js   one file per screen
    99-main.js      router, poll loop, theme, language switch
  i18n/
    uk.json         every word the page shows, key -> text (the default)
    en.json         the same keys in English
  sim/
    mock.js         the simulated board: state and routes
                    used by the simulator only, never shipped to the board
tools/gen_pages.py  inlines web/index.html + app.css + js/* into PAGE_APP (gzip),
                    keeps the build-time feature gating
tools/build_sim.py  the same app plus sim/mock.js, as one HTML file (the artifact)
tools/check_i18n.py the dictionaries agree, and every key the code names exists
```

## Languages

The page speaks Ukrainian (default) and English; the switch in the header
(EN / UA) is per browser (`localStorage` `mo-lang`, or `?lang=en` in the
address) and reloads the page.

- Every word is in `web/i18n/<lang>.json`, flat `"area.name": "text"` keys:
  `nav.*` tabs, `m.*` header and restart bar, `c.*` shared words, `s.<state>`
  and `w.<code>` for `/api/health`, then one prefix per screen (`h.*` Огляд).
  `{0}`, `{1}` are arguments.
- `t(key, ...args)` (`00-util.js`) looks the key up in the chosen language,
  then in Ukrainian, then returns the key itself. Never at script load: the
  English file arrives later (`i18nLoad()` in `start()`), so a screen's
  `title` is a key and the router translates it.
- Static text in `index.html` is `data-i18n="key"` / `data-i18n-aria="key"`,
  filled by `i18nLoad()`.
- The board's page inlines only `uk.json`; `en.json` is a separate gzipped
  file at `GET /i18n/en.json`, fetched by an English viewer only. The
  simulator inlines both.
- The catalog's names are words too, `k.*`, put in by `i18nCat()` (`20-kit.js`)
  once, right after `/api/catalog`, so the screens read `name`, `label`,
  `group` and `unit` as before. The key comes from the id: `k.set.<setting>`
  and `k.set.<setting>.<column>`; an enum row `k.<dom>.<v>`, where the
  firmware's `SEnum::dom` names the things it lists (`sig`, `style`, `scene`,
  `ble`…, one word each however many settings list them), or
  `k.set.<setting>.<v>` without one; a signal `k.sig.<v>`, else its name in
  the Raise list `k.raise.<v>`; `k.rgroup.<id>` and `k.unit.<unit>` for the
  Raise rows' section and unit (ids and terse English units from the
  firmware); `k.layout.*`, `k.style.*`, `k.scene.*`, `k.con.*`, `k.dest.*`,
  `k.action.*`, `k.pkey.*`, `k.class.*` for the other lists. The settings
  carry no `label` any more; `tools/check_i18n.py` fails on a catalog name with
  no words (it derives the keys the same way from `build/catalog.json`).
- The firmware sends codes, not prose (English stays in the log only):
  `/api/health` chains carry `code` (`w.<code>`), error replies are codes
  (`e.<code>`, see the API contract), and the page has the words.
- English is written for someone who has never seen the Ukrainian: natural
  phrasing, not word for word.
- `tools/check_i18n.py` (CI): same keys in both files, same `{n}` arguments,
  no empty value, no literal key missing; warns on unused keys and counts the
  Ukrainian still in `web/js` (`--strict` fails on it once every screen is
  migrated).

With `?sim`, or in the simulator build, `10-api.js` routes `call()` to the mock. The board build never contains `mock.js`.

## API contract

These are the changes only. Everything not named stays as it is.

**New, and what each one replaces:**
- `GET /api/catalog`: the lists from rule 2, built from the firmware's own tables:
  - `MediaScreen` layout and style names
  - `MediaPane` scenes
  - `Rav4Profile` codes
  - `AffaTypes` keys
  - `KeyAction` classes
  - the console command table
  - `RaiseTargets`

  It replaces the following, which go:
  - `GET /api/canbox/catalog`
  - the `actions` arrays in `/api/affamap`, `/api/knobs` and `/api/macros`
  - `styles` in `/api/media`
- `GET /api/logs?since=N` returns `{lines:[{n,t,l,tag,msg}], next, lost, more, dropped, boot}` from a LogBus ring with sequence numbers. It replaces `/ws`, `WsStream` and the K/L/T/V/J web-sink prefixes. `GET /api/logs` without `since` keeps today's file list.
  - `n` is the line's number (1, 2, … since boot); `t` is uptime in ms; `l` is `ERR`…`TRC` (a line without a level is `INF`); `tag` is one of `logTags`; `msg` keeps the line's own `[cat]`.
  - `next` is the `since` for the next poll. `lost` counts lines the ring evicted before this reader got to them (the page shows a marker; on the first poll, `since=0`, it is only the boot lines already gone). `more:true`: the answer was capped (200 lines or ~8 kB), ask again at once.
  - `dropped` counts lines the full ring refused since boot; the page marks each increase. `boot` changes every boot (`esp_random()`); when it changes, the page restarts at `since=0` with no marker. A `since` past the newest line reads from the start as well.
  - The ring is the LogBus's own (`LogRing.h`): sent lines stay in it until a new line needs the room. No second buffer.
- `POST /api/reboot` replaces the reboots that happen as a side effect of `/api/settings`, `/api/wifi` and `/api/display`. Those now save and answer `{ok:true,pendingReboot:true}`, or `{ok:true}` when nothing that is read only at boot changed. `/api/reboot` answers `{ok:true}`, then restarts (reply, 300 ms, restart: the one helper `/api/factory` and `/api/can/role` use as well).
  - Boot-only: `bleMode`, `canRxPin`/`canTxPin`, `canRole`, `mdnsOn`, `apPass`; the WiFi networks; the panel `type` and `funcReg`.
  - `GET /api/settings` shows the saved values. WifiManager keeps boot copies of the first network, the AP password and whether BT is up, so those never reach the running WiFi before the reboot. The second network is the exception, on purpose: `setNet2` stages it and WifiManager takes it at its next AP retry, so `ssid2`/`pass2` alone answer no `pendingReboot`.
- `GET/POST /api/map` and `POST /api/map/test` replace:
  - `/api/signals` (GET and POST)
  - `/api/signals/unpinall`
  - `/api/emitter`
  - `POST /api/canbox` (`cmd28`, `doorsWithIg`, `steeringInverted`, `huTextToDisplay`)
  - the scale fields in `/api/canbox/tuning`

  The mapping lives in the NVS blob `decode` (`Vehicle::DecodeTable::pack`, at most 48 CAN rows and 16 constants, 1187 B) on every env, and is hot-loaded through Staged (the two buffers in PSRAM where there is some). No blob is the factory table, `decode_defaults.inc`; `POST /api/map {reset:true}` erases the key. A save that does not reach NVS answers 500 with a text body.
  - `GET /api/map`: `{send, uart, units:{fuel,trip}, rows:[{id, src, can|null, k, v, how, left?, raw?}]}`. A row's name, unit, range and `fw` are the catalog's `raise` row with the same `id` (the signal's name); `k` and `v` have the decimal point placed. `raw` only while the ID table (`/api/can/sniff`) has the id.
  - `POST /api/map`: any of `send`, `units`, `rows:[{id,src,k,can}]` (16 at most, one per field), `reset`. Everything is checked before anything is applied. 400 bad field or a field twice, 404 unknown id, 409 a field with no signal, the UART off for `send`, or a full map.
  - `POST /api/map/test {id,value}`: pins the value for 30 s and sends the field's message once; `{ok,bytes}`. 409 when protocol 112 has no field for it (`fw:false`), the UART is off, `0x28` carries the other form, or the message's gate field (odometer, rear radar, TPMS, hybrid) has no value yet.
- `GET /api/config` and `POST /api/config` (settings to a file) are the only addition that replaces nothing. They go last and can be dropped.

**Changed parameters, no new routes:**
- `POST /api/wifi`:
  - takes `ssid` and `pass`, `ssid2` and `pass2` (JSON), either pair or both; `/api/cloud` loses `wifi2` in slice 8;
  - an empty password means "unchanged" while the name stays the same; a new name with no password is an open network. The second network goes through `WifiManager::setNet2` (NVS `net2`), which applies the same rule, so `/api/cloud` `wifi2` inherits it until slice 8 deletes that path;
  - `pendingReboot` only when the first network's name changed or a non-empty password differs from the stored one;
  - errors, `text/plain` 400: `ssid? or ssid2?`, `ssid: 32 characters at most`, `pass: 8 to 64 characters, or empty`.
- `POST /api/settings` takes `apPass` (the AP password): empty is unchanged, else 8 to 63 characters or 400 `apPass: 8 to 63 characters`.
- `POST /api/display` answers 400 `type: unknown panel` for a type the build does not have.
- `GET /api/affamap?live=1` returns `{last:{seq,key,wire,hold,action,dests[]},unmapped,events}`. Without `live`, `last` rides along. `g_lastKeyLine` goes.
- `POST /api/display/key`: `wire=1` sends the key frame onto CAN B (`KeySource::Wire`) instead of into the routing.
- `POST /api/media`:
  - `bits` (576 hex, `MediaPane.h` format: 6 bytes per row, MSB = left pixel) is the custom 48×48 pane picture. It is stored (NVS `paneBits`) and selects the `custom` scene. A bad string answers 400 `bits: 576 hex characters`; `pane:"custom"` before any picture answers 409 `draw the custom picture first`;
  - `scroll`, the menu layout's arrows parameter, takes only the catalog's `arrows` values (`0`, `7`, `11`, `3`); anything else answers 400 `scroll: 0, 7, 11 or 3`. `POST /api/display/menu` checks its `scroll` the same way;
  - every `layouts[].params` name is accepted here and reported by `GET /api/media` (`tools/check_nav.py` checks).
- `GET /api/media` gains `clockSrc` (`hu`, `cts`, `manual` or empty) and `clockAt` (`HH:MM` of that set, or empty). `pane.bits` is the custom picture while the `custom` scene is live.
- `GET /api/settings` gains `ssid2`.

**Deleted with no replacement:**
- `/api/test` (duplicate of `/api/display/key`)
- `/api/media/paint`
- `/api/display/fsgeom` and `/api/display/raw`
- `/api/link*` and `/api/can/role` on t2can
- the bench-only `/api/can/{level,pintest,txwatch,pads,alerts,selftest,loop,probe,mode,listen,timing POST}`, reviewed one by one against `/wiggle`
- `/api/keymap*` where `FEATURE_MATRIX` is 0
- the old pages:
  - `*Assets.h`
  - `/display`, `/canbox`, `/vehicle`, `/car`, `/knobs`, `/macros`, `/res`, the index

`/wiggle` stays.

**What slice 8 did with that list (2026-10-17):**
- Deleted as listed: `/api/test`, `/api/media/paint`, `/api/display/fsgeom` and `/raw`, the old pages, `/ws`; also `GET /api/canbox` (nothing read it) and `/api/routing` `classes` (the page reads the catalog's).
- `/api/can/role` is gone on every env: `POST /api/settings {canRole}` already set the role (boot-only, `pendingReboot`); its t2can refusal moved there (400 `canRole: this board's car bus is CAN A`).
- `/api/link*` stay on the env that has the link (devkit), now gated by `FEATURE_LINK` alone (they were also under `FEATURE_VEHICLE_CAN`).
- Bench: `/api/can/alerts` and `/api/can/probe` (and the 0x7FF probe transmitter in `main.cpp`) are deleted. Kept, because `docs/bench.md` "CAN instruments and their traps" is the current procedure that uses them: `level`, `pintest`, `pads`, `txwatch`, `mode`, `listen`, `timing` (GET and POST), `selftest`; `loop` is `/wiggle`'s.
- `/api/keymap*` is compiled only where `FEATURE_MATRIX` is 1 (`direct`), with no UI; `/api/res*` likewise (`FEATURE_RESISTIVE_KEYS`). Both are in `docs/BACKLOG.md`.

## Slices

| # | Slice | Contains |
|---|---|---|
| 1 | Web source tree | `web/` from the simulator, split into modules; `gen_pages.py` builds `PAGE_APP` and `build_sim.py` builds the artifact; served at `/ui` (temporary) next to the old pages. No API changes. The page already reads `/api/catalog` (served by the mock in the simulator). |
| 2 | Catalog | `/api/catalog` from firmware tables (`src/net/Catalog.cpp`); the simulator serves the same JSON (`build/catalog.json` from `test_catalog`). The duplicates it replaces stay until slice 8: the old pages still read them. |
| 3 | Logs | LogBus sequence ring, `/api/logs?since`, the Service log. |
| 4 | Settings, reboot, WiFi | `/api/reboot`; no reboot as a side effect; `ssid2`; empty-password fix; `apPass`. |
| 5 | Keys | `affamap.last`, `key?wire=1`, `usb` in `/api/routing`. Delete `g_lastKeyLine`. `/api/test` stays for the old pages until slice 8. |
| 6 | Media | `bits`; arrows validated; `clockSrc`/`clockAt`; layout parameters from the catalog. `/api/media/paint` stays for the old page until slice 8. |
| 7 | RAV4 map | `RaiseTargets`, the NVS blob `decode`, `/api/map` and `/api/map/test`. Deleted: `rpmMul`/`rpmDiv` (the wire is a fixed x4) and the `cmd28` side effect in the old /canbox climate button. Kept, because a row cannot express them (they act on the wire, and a test value never passes a row): `steerDiv`, `steerClamp`, `tripFields`, `histLen`, `radarInvert`, and `cmd28` as a setting. `doorsLen` defaults to 2, and a stored 1 from before is migrated once. The routes the map replaces stay for the old pages until slice 8. `docs/signal-glossary*` (f3fc721) was checked: its broadcast picks are the defaults already; it adds only diagnostic sources. |
| 8 | Cutover (done 2026-10-17) | `/` serves the app. Delete the old pages, `/ws` and `WsStream`, and the dead routes, `/api/test` among them. Delete `webSink` in `main.cpp` with its K/L/T/V/J re-tagging (`isKeyEvent`, `isHuLine` stay only if the serial and flash sinks still need them), and `g_ws` with its `[hb] ws=` field. Delete what `/api/catalog` replaced (slice 2): `GET /api/canbox/catalog`; `actions` in `/api/affamap`, `/api/routing`, `/api/knobs`, `/api/macros`, `/api/macros/live`, `/api/res` and `/api/keymap`; `triggers`/`effects` in `/api/macros`; `styles` and `pane.scenes` in `/api/media`; `names` in `/api/loglevel`. Delete `wifi2` from `/api/cloud` (GET and POST; slice 4 moved it to `/api/wifi`). Delete `/api/media/paint` (the old page's "draw once"; it posts the info layout as a menu). Delete what `/api/map` replaced (slice 7): `/api/signals` (GET and POST), `/api/signals/unpinall`, `/api/emitter`, `POST /api/canbox` (`cmd28`, `doorsWithIg`, `steeringInverted`, `huTextToDisplay`), and `/api/messages*` unless the Service page still reads them; decide then whether the kept tuning fields stay in `/api/canbox/tuning`. Final ledger. |
| 9 | On the board | The owner's OTA and car check. **2026-10-17: OTA-flashed 9383791 to the T-2CAN, confirmed after 60 s; desk check passed (no reboot, WiFi steady, AMS/ANCS/CTS start, clock from phone). Open: BLE drops with 0x208 every few minutes, more often under steady HTTP polling (a 5 s log/health poll made it ~1/min, min heap 22 kB). Car check pending; read the log ring over WiFi afterwards before any reboot.** |

## Ledger

Counted with `grep -c 'route("' src/net/*.cpp`, `cat src/net/*.cpp src/net/*.h | wc -l` (gen/ not included), and `cat web/index.html web/app.css web/js/*.js | wc -l` (`web/sim/*.js` in brackets). Per-env route counts include main.cpp's two `/api/cloud`.

| After | Routes | src/net lines | web lines | Notes |
|---|---|---|---|---|
| 2e04085 (start) | 129 | 8738 | 0 | 129 registrations across all features. `ROUTE_MAX` is 128; the per-build count is not measured yet. |
| slice 1 | 130 | 8742 | 884 (+700 sim) | +1 temporary route `/ui`. PAGE_APP is 34.4 kB gz. Per env, t2can has 122 of 128 routes. |
| slice 2 | 131 | 9226 | 884 (+635 sim) | +1 `/api/catalog`. Flash +9.1 kB on t2can, +512 B static RAM. The duplicates it replaces are deleted in slice 8; the old pages still read them. |
| slice 3 | 131 | 9289 | 892 (+640 sim) | `/api/logs?since` reads the existing ring (LogRing, 32 kB, no second buffer). Flash +1.8 kB on t2can. `/ws` goes in slice 8. |
| slice 4 | 132 | 9384 | 900 (+659 sim) | +1 `/api/reboot`, which replaces 3 reboots that happened as a side effect. t2can has 124 of 128 routes. Flash +3.2 kB on t2can. |
| slice 5 | 132 | 9449 | 905 (+667 sim) | No new route. Live key report (snapshot, 8 B), CAN B key (`wire=1`), and one dest builder for routing and health. `g_lastKeyLine` deleted. Flash +1.4 kB on t2can. |
| slice 6 | 132 | 9518 | 910 (+678 sim) | No new route. `custom` pane scene (`Staged<PaneBits>`, NVS `paneBits`; the pane borrows the live buffer), `clockSrc`/`clockAt`, `scroll` checked against `arrows`, `check_nav.py` checks layout params. Flash +2.4 kB, static RAM +736 B on t2can (the picture's two staged buffers, 576 B, are most of it). |
| slice 7 | 135 | 9848 | 912 (+686 sim) | +3: `GET`/`POST /api/map`, `POST /api/map/test`; they replace `/api/signals` (2), `/api/signals/unpinall`, `/api/emitter` and `POST /api/canbox`, which go in slice 8. t2can has 127 of 128 routes. `RaiseTargets` (102 rows, 8 B each), `DecodeTable` with its NVS blob `decode` (max 1187 B), test pins (30 s, car task). Flash +17.0 kB, static RAM +152 B on t2can (the two map buffers, 2 x 1.3 kB, are in PSRAM); devkit flash +16.8 kB, static RAM +160 B, plus 2.6 kB of heap for the map. |
| slice 8 | 112 | 6105 | 920 (+694 sim) | Cutover. `/` serves PAGE_APP; `/ui` gone. Deleted: 8 old pages (`*Assets.h`, 3,077 lines) with their 7 routes, and `/ui`; `/ws`, `WsStream` (233 lines), `g_ws`, `webSink` with its K/L/T/V/J re-tagging, the `[hb] ws=` field and the `ws*`/`boot*` fields of `/api/status`; `/api/test`, `/api/media/paint`, `/api/display/fsgeom`, `/api/display/raw`, `/api/signals` (2), `/api/signals/unpinall`, `/api/emitter`, `GET` and `POST /api/canbox`, `GET /api/canbox/catalog`, `/api/can/role`, `/api/can/alerts`, `/api/can/probe`, `/api/can/raw` (15; 23 registrations with the pages and `/ui`). `/api/keymap*` (3) compiled only with `FEATURE_MATRIX`. Deleted fields: `actions` in `/api/affamap`, `/api/routing`, `/api/knobs`, `/api/macros`, `/api/macros/live`, `/api/keymap`; `classes` in `/api/routing`; `triggers`/`effects`; `styles` and `pane.scenes` in `/api/media`; `names`; `wifi2` in `/api/cloud`. Deleted as dead once their caller went: the hu-text-to-panel echo, the `serialLog` switch (the [hu] echo is always on Serial, as by default before), the 0x7FF probe transmitter, `onRawEdge` and `KeyEngine::onRaw`, `Canbox::trial*`/`excluded*`, `AffaMailbox::postRaw`/`Op::Raw`/`postFullscreenGeom`, `App::Car::unpinAll` (and `State::unpinAll`), `Vehicle::groupName`, `Keys::className`; 3 native tests that only covered them (598 -> 596 with the new `test_features`). **New: `features` in `/api/catalog`** (display, ble, ancs, vehicle, can2, capture, knobs, matrix, res, link, logfs, usb, uart), from the same FEATURE_* the routes are gated by; every screen fetches only routes that exist and hides cards whose feature is off (devkit/direct: no CAN A, no capture, no flash log; direct: no panel). The simulator takes `?env=` (and a select in its panel): `gen_pages.py` writes `build/features/<env>.json` from each board build, `test_catalog` writes `build/features.json` (id -> macro), `build_sim.py` combines them; the mock 404s absent routes. The "Налаштування у файл" card is gone until `/api/config` exists (BACKLOG). The cloud ring is now a LogBus sink (lines as logged, no K/L prefix). The per-frame raw CAN stream (`/api/can/raw`, `/ws` only) is deleted, not moved to the log: capture files and `/api/can/ids` cover CAN analysis, and no bench.md procedure used it. The `[hu] R` echo stays on Serial only (`LogBus::serialOnly`), as under `webSink`: `/api/logs` and the cloud skip it, since `[hur]` carries the same frame decoded. `POST /api/settings` checks the CAN pins and `canRole` before applying anything. Per env: t2can 127 -> 102, t2can_usbhid 127 -> 102, devkit 110 -> 86, direct 77 -> 60. `ROUTE_MAX` 128 -> 112. Flash/static RAM vs slice 7: t2can -93.9 kB / -6.6 kB, t2can_usbhid -93.7 kB / -6.6 kB, devkit -89.0 kB / -6.6 kB, direct -62.5 kB / -6.3 kB. `gen_pages.py` lost the nav, the log pane and the feature gating (PageGz is the same for every env); `check_nav.py` proves `/` is PAGE_APP and that the app's links (`/wiggle`) are in the binary. |
| **branch** (2e04085 -> slice 8) | 129 -> 112 | 8738 -> 6105 | 0 -> 920 (+694 sim) | Per env: t2can 121 -> 102 routes, devkit 104 -> 86, direct 71 -> 60. Flash: t2can 1,645,501 -> 1,620,873 B (-24.6 kB), t2can_usbhid 1,677,101 -> 1,652,509 (-24.6 kB), devkit 1,655,237 -> 1,635,893 (-19.3 kB), direct 1,279,981 -> 1,278,653 (-1.3 kB). Static RAM: t2can 141,616 -> 136,816 (-4.8 kB), t2can_usbhid 153,728 -> 148,928, devkit 109,932 -> 105,140, direct 87,372 -> 82,180. The whole UI is now one page (35.5 kB gz) where nine were (114 kB gz). |

## /api/catalog shape

The simulator mock and the firmware both answer exactly this shape:

```json
{
  "v": 1,
  "features": {"display":true,"ble":true,"can2":false,"capture":false,"…":"one bool per FEATURE_* (slice 8)"},
  "layouts":  [{"id":"info","name":"Інфо","cols":8,"rows":3,"pane":true,"clock":false,
                "params":["style","scrollMsTitle","scrollMsArtist","splashMs"],
                "panels":["carminat"]}],
  "styles":   [{"id":"classic","name":"класичний","animated":false}],
  "scenes":   [{"id":"media","name":"медіа"}],
  "enums":    {"arrows":[{"v":0,"name":"без стрілок"},{"v":7,"name":"▲"},{"v":11,"name":"▼"},{"v":3,"name":"▲ і ▼"}],
               "icons":[…], "src":[…], "fmt":[…], "ledIdle":[…], "ledReact":[…], "logLevels":[…]},
  "panels":   [{"id":"carminat","name":"Carminat","caps":["text","menu","info","fullscreen","popup","pane"]}],
  "bleModes": [{"id":"off","name":"Вимкнено"}],
  "classes":  [{"bit":0,"name":"треки"}],
  "dests":    [{"id":"uart","name":"ГУ","built":true}],
  "actions":  [{"id":"VolUp","class":1,"hu":"01"}],
  "panelKeys":[{"id":"VOL_UP","name":"Гучність +","wire":"0x0003"}],
  "huKeys":   [{"code":"16","name":"Ok"}],
  "console":  [{"cmd":"menu","name":"Меню","fields":[{"n":"header","label":"заголовок","max":26},
                {"n":"scroll","label":"стрілки","enum":"arrows"},{"n":"ms","label":"мс","num":true}]}],
  "raise":    [{"id":"speed","group":"Рух","name":"Швидкість","unit":"км/год","cmd":"7D 03",
                "kind":"n","min":0,"max":250,"dec":0,"fw":true}],
  "logTags":  ["KEY","PNL","BLE","HU","CAN","CAP","MAC","NET","SYS"]
}
```

- **Field meanings:**
  - `kind` is `n` (number) or `b` (yes/no).
  - `fw:false` is a car value protocol 112 has no field for (only the battery, `batt_v`): `why` says so, `cmd` is empty, and the page links the experiment that borrows another field.
  - The scene index is the position in `scenes`. `custom` is a real `MediaPane` scene, last in the enum (slice 6).
### Catalog decisions after the slice 1 review (2026-10-16)

- **Layout parameters are data.** `layouts[].params` holds field objects in the same shape as the console fields, rendered by one renderer:
  - a number: `{"n":"scrollMsTitle","label":"…","num":true,"min":0,"max":5000,"step":50,"hint":"0 = стоїть"}`;
  - a list: `{"n":"style","label":"Стиль","enum":"styles"}`.
- **A range is not an enum.** Use `{num:true,min,max}`; the page draws a select when the span is 8 or less. That covers the menu row and the message-box buttons.
- **`enums` entries:**
  - `arrows` entries carry `up` and `down` booleans.
  - More enums: `mediaSrc`, `canRates`, `capModes`, `fuelUnits`, `tripUnits`, `mapSrc`, `macroTriggers`, `macroEffects`, `ledOrder`.
- **`actions`:**
  - Ids are the firmware's (`KeyAction.cpp`: `None`, `SrcNext`, `SrcPrev`, …).
  - Each row carries a `name`. `None` is the "nothing" row, with name `—`.
- **`panelKeys`:**
  - Each entry carries `src`: `wheel`, or `knob` with an empty `wire`.
  - The CAN B key sender uses only the `wheel` keys.
- **`bleModes[]`** carries `ams:true` where AMS, ANCS and CTS run.

### Unchanged endpoints: the firmware is the spec

The simulator mock answers exactly what the firmware answers today. Examples:
- `/api/status` `mode` is `sta` or `ap`.
- `/api/cap/settings` `ring` is a bool.
- Error bodies are `text/plain` codes, one refusal a line: `[key: ]code[ args]`
  (`busy`, `param exp`, `pulseMs: number`, `brake: between 0 100`,
  `not_built display`). The page shows `e.<code>` from `web/i18n` with the args
  (`errLine()`, `20-kit.js`), and a code it has no words for as it came. A
  missing or malformed request parameter is `param <name>`: the page never
  sends one, and an API caller reads the handler. `tools/check_i18n.py` fails
  on a reply code with no `e.<code>`.

These fields are added; no route is added for them:
- `GET /api/status` gains `fw` (`MEGAOPEN_VERSION`), `build` (`MegaOpen::buildId()`, the `tools/version.py` stamp) and `apName` (the AP's full name, with the MAC suffix).
- `GET /api/health`:
  - every chain carries a stable `code` and no prose: the page has the words (`w.<code>`, see Languages);
  - a `hu` chain: `state`, `code` and `rxFrames`. It is `ok` while the head unit sent a frame within `HuMediaSource::linkTimeoutMs()` (15 s). Builds without the panel have no `HuMediaSource`: after the first frame they report `idle` with `hu_seen` (a frame since boot, recency unknown), never `ok`.
  - The codes: `panel_off`, `panel_unregistered`, `panel_ok`; `media_off`, `media_no_panel`, `media_no_phone`, `media_no_track`, `media_ok`; `clock_off`, `clock_phone`, `clock_hu`, `clock_waiting`, `clock_no_source`; `keys_ok`, `keys_none`; `ancs_ok`, `ancs_no_phone`, `ancs_waiting`, `ancs_off`; `hu_off`, `hu_silent`, `hu_quiet`, `hu_ok`, `hu_seen`; `phone_ok`, `phone_ble_off`, `phone_advertising`.
- `POST /api/macros` errors are `text/plain` `"<index>: <code>"` (`3: macro_incomplete`). The page marks the row whose number leads the message.

### Open contract items, each tagged with the slice that settles it

- **Slice 2 (settled):**
  - Served: `mediaSrc[].pick` and `mediaSrc[].ams`, `macroTriggers[].taps`, `macroEffects[].field`. Booleans are always present (`false`, not missing).
  - `/api/knobs` and `/api/macros` keep `actions` until slice 8, which deletes them with the old pages.
  - The literals `'off'` (bleMode), `'auto'` (pane carousel) and `'custom'` (scene) stay in the page. They are the plan's fixed ids, not lists.
  - `raise` is `[]` until slice 7; `mapSrc` sits in `Catalog.cpp` until slice 7 moves it beside the decode map (done: `Vehicle::MAP_SRC`, `DecodeTable.h`).
  - Rows the build compiled out are absent (no display: no layouts, styles, scenes, panels, wheel keys, console); `dests[].built` says which sinks exist, and `usb` is listed only where `FEATURE_USB_HID` is on.
  - `huKeys` is the whole `Rav4Catalog` table, the knob codes `81`–`86` included. `actions[].hu` is the bare code (`Mute` is `07`; the sink sends it as a long press).
- **Slice 4 (settled):**
  - `/api/health` `why` texts stay English in the firmware. The chains carry a stable `code`; the page owns the Ukrainian text per `code` in one table (`WHY_T`, `web/js/40-home.js`) and falls back to `why`. *Superseded by Languages: `why` is gone, the words are `w.<code>` in `web/i18n`.*
- **Slice 5 (settled):** `/api/routing` `dest` is exactly the catalog's `dests` (`usb` where `FEATURE_USB_HID`; NVS `rtUsb`), each `{mask,live,why}`; `live` = in the chain and switched on. `affamap.last.dests` = mask, `live`, and the sink's own verdict (UART needs a code; BLE and USB take no Long). A swallowing macro and a disconnected phone or USB host are not seen.
- **Slice 6 (settled):**
  - `/api/media` gains `clockSrc` (`hu`, `cts`, `manual` or empty) and `clockAt` (`HH:MM`). The ui task records each set it applies: the head unit's 0x9D and the phone's CTS while `autoTime` is on, and `/api/display/clock` with a valid `HHMM` (the web posts it; the ui task only records it and puts it in the header). Published in `UiStatus`.
  - The menu scroll byte stays in the existing `scroll` field. Valid values are `0x00`, `0x07`, `0x0B` and `0x03`, per `CarminatConstants.h`, from one table (`Catalog.cpp` `kArrows`, served as `enums.arrows`, checked by `Catalog::arrowsKnown`). The firmware comment and the old page's `0x0C` are fixed. Defaults unchanged: `/api/display/menu` `0x0B`, the now-playing menu layout `0x00` (not a list).
  - `custom` is `MediaPane::Scene::Custom`. The carousel skips it (a still picture chosen on purpose, not a decoration). The picture travels httpd → ui as a `Staged<App::Ui::PaneBits>` (apart from `MediaSettings`, which httpd copies onto its stack) and lives in the pane, so the `paneMirror` copy composes it for `GET /api/media`.
  - `GET /api/media` `preview` is `MediaScreen::compose` on the newest settings, the same call the ui task posts: text 8×1 (rows 2-3 empty), info 8×3, menu and fullscreen 26×3. Correct for every layout. `/api/media/paint` posts info as a menu; it goes in slice 8.
- **Macro errors:** the index is 0-based, the slot as the firmware counts it.

- **Text the UI may still own:** hints and explanations, keyed by `id`. The page shows the bare `name` when a hint is missing, so a new row in the firmware never needs a UI edit.
