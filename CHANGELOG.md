# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions follow
`MEGAOPEN_VERSION` in `src/Version.h`.

## [Unreleased]

### Changed
- «Оновити з GitHub» (check or install) pressed while the log push is uploading a part no longer answers «інше завантаження ще триває, спробуй за хвилину»: it queues («у черзі»), goes next (the push cannot take TLS again before it) and starts by itself, at most a minute later (`TlsJob::reserve`).

### Fixed
- «Мапа» on a wide screen (900 px and up: the head unit, a PC): the tabs and the table were scattered over the page's columns, the table squeezed into a strip on the right. Each tab's card now spans the full width.

## [1.6.6] - 2026-10-28

### Added
- An icon: the page's favicon (the board no longer answers a browser's `/favicon.ico` with 404) and MegaOpen Companion's launcher icon, one picture (`art/icon.webp`) cut by `tools/gen_icons.py`.
- A USB cable test from a PC: `python tools/usb_link.py cable [seconds]` sends pings of every size up to 1024 bytes over the board's USB link and checks each echo byte for byte; it counts lost and wrong echoes, CRC errors and drops (reconnecting), prints the speed and latency, and ends GOOD or BAD. No firmware change, no WiFi. On the bench: 4861 echoes in 30 s, none lost, ~143 kB/s.

### Changed
- One page for the head-unit mapping: «Мапа» (`#map`, a card on «Авто») with three tabs, Сигнали | Поля ГУ | Сповіщення (`#map:fields`, `#map:alerts`). Сигнали is the old «Авто — наживо» table (source chips, ✓, 🔔, «Заводські прив'язки»), and each signal row now names the head-unit fields that take it («→ поле ГУ», a link to the field). Поля ГУ is the old RAV4 card from «Авто» (protocol, «Слати в ГУ», units, «Звідки», test ▶, «Прогін селектора»); «змінити в таблиці сигналів» and the other signal's name open that signal's source editor on Сигнали, without leaving the page. Сповіщення is the alert rules table; 🔔 lands there on the new row. `#live` is gone. Web only (docs/design/map-page.md).

### Fixed
- «Авто» no longer shows «нема зв'язку» after leaving it for another page while its gears or climate-diag values were still loading.

## [1.6.5] - 2026-10-28

### Added
- The pictures are back on the panel, as scenes on «Екран»: «тризуб», «тризуб + годинник» (a smaller tryzub with the time, HH:MM, under it, live) and «Renault» (the losange). They were AffaDisplay's bench console's pictures (the tryzub its default), gone with those examples (AffaDisplay 8e0d7ae); the bitmaps are copied from its examples/shared/nav_images.h (src/display/PaneImages.h). Still pictures, so not carousel stops, like «свій малюнок».
- The settings file carries the panel's own picture («свій малюнок», `picture`, 576 hex) when one is drawn: a reset or another board no longer loses it. An import checks it with the rest and the preview says it is there.

## [1.6.4] - 2026-10-28

### Added
- A status snapshot to GitHub: `/api/status` and `/api/health` as one JSON (`{uptimeMs, status, health}`), `boot-N/status-MMMM.json` (uptime minutes) right after the boot's first push and every 10 min, for tools to read without regexes (docs/design/logging.md §5).

### Changed
- Where a car signal comes from (a CAN row, a constant, nothing) is edited in one place, the signals table (Авто — наживо): its source chip opens the editor, «Запам'ятати / показати зміни» included, for every signal, not only the ~60 a head-unit field has (`POST /api/map` takes any signal's name; `GET /api/map?sig=<name>` gives one signal's binding). The head-unit map's editor keeps «Звідки» only; under «свій сигнал» it says where that signal comes from, with «змінити в таблиці сигналів», which opens it there (step 4 of docs/design/hu-mapping.md, first half).
- «Заводські прив'язки» (the signals' factory bindings) moved from the head-unit card to the signals table, the page where bindings are edited; it leaves the head-unit field map alone.
- The head-unit protocol is one interface (`Canbox::FieldProtocol`, step 5 of docs/design/hu-mapping.md): its fields and their defaults; the field map, `/api/hu/fields`, `/api/map` and the catalog (`proto`) read the active one, and the page shows «Протокол ГУ: Raise 112 (Toyota RAV4)». A stub protocol in the host tests runs the map unchanged.

### Fixed
- After the update to 1.6.3, empty alert rows and an empty car-screen slot came back as `fuel_pct` (Бак, %), and the live page could not add a rule («all 16»). An empty row was saved as the build's signal count, which moves each time a signal is appended; 1.6.3 appended `fuel_pct`. Saved, an empty row is now a fixed 0xFF (the alert table's format 2, `carSigV` for the car screen); a table or slot saved by 1.6.3 or before reads its old marker, and a `fuel_pct` there, as empty. Checked on the bench: saved on 1.6.2, wrong on 1.6.3, right on this build.
- A status snapshot GitHub keeps refusing no longer stands in front of the log parts: the next one waits 10 min either way.
- The head-unit field map takes at most 29 rules of the owner's (`rows_max 29`): the protocol's defaults share the 32-rule map, and the last rules were dropped silently. The 1.6.1 switches' migration keeps them in NVS if the map is full.
- A settings file exported when the board had no memory for the field map left `"fields":""`, which its own import refused; the key is left out now.
- The status snapshot reads the settings under their lock, as the web routes do.
- Signals page: a second source chip closes the open editor and opens its own (it did nothing); «Запам'ятати» starts over each time an editor opens.
- A coredump pushed to GitHub could not be decoded when the redaction had blanked a secret in it (`Invalid core dump CRC`): its CRC-32 is written again over the redacted bytes. Checked on the bench's redacted dump of boot 122 (re-sealed offline, then decoded).

## [1.6.3] - 2026-10-28

### Changed
- The head-unit experiments are field-map rules now (step 2 of docs/design/hu-mapping.md, finished): the switches «Експерименти ГУ» and «Напруга АКБ у полі ГУ» are gone. The fields they fed start off and are switched on in the map table's «Звідки» like any other: coolant (0x7D 09), the PRND probe (0x7D 06) and «Бак, %» (0x7D 04 D4-D5, F2 08 while off) → «свій сигнал»; the battery → another field's «Звідки» → `batt_v` ÷10 (rounded); the board's range and consumption → «Запас ходу» ← `range_calc` etc. On the first boot the old switches (NVS `huExp`) become those rules once (`W [hu] experiments 0x.. -> field map: saved`); a settings file from before brings its `exp` the same way and a new one carries `hu.fields`. A test send of a field that is off answers «поле вимкнене».
- The tank in per cent is a car signal, `fuel_pct` (x10, `fuel_level` over 60 L, derived).
- `[stk]` also shows `httpd`, the web server's stack left.

### Fixed
- A crash of the web server (`task=httpd`, a stack overflow, two coredumps on the bench): the head unit's settings, now with the field map, were copied onto httpd's 4 kB stack by every map route and the settings file, and the file's field-map buffers sat there too; they are per-request (PSRAM) now. And httpd has 6 kB (`HTTPD_STACK`, TaskPlan.h): a settings-file import, the deepest route, left 256 B of the 4 kB, and in 1.6.2 it overflows. With 6 kB, 2.3 kB stay free after every map and settings route; about 1.7 kB less heap.

## [1.6.2] - 2026-10-28

### Added
- `[drive]` also carries the fuel: litres (0x5C5, and the cluster's 0x715 copy) and the low-fuel bit, after a low-fuel alert with a full enough tank (2026-10-27).
- Авто › «Передачі»: live once a second, how the board takes the gear: speed, rpm, the ratio, the gear it reads, clutch and neutral; the table (editable) next to what the learner collected near each value, and «Це N-та» to put the current ratio into the table while driving steadily in that gear. `GET /api/gears`.

### Changed
- The log, step 1 of docs/design/logging.md: the level (`logLevel`) is saved and survives a reboot; per-tag levels (`logTags`, e.g. `hu=TRC knobs=DBG`, tags register themselves on first use); and per sink, the lowest level each takes (`logWeb`, `logFlash` at most DBG, `logPush`, `logSerial`). The LOGx macros look a tag up once per call site and format nothing for a line that will not be written.
- The log, step 2: every line the firmware writes goes through LOGE..LOGT with a level and a tag (212 calls converted, the rest by hand; the text after the tag is unchanged, so `[ota] CONFIRMED` still reads the same, now as `I [ota] CONFIRMED`). `tools/check_rules.py` refuses a `LogBus::printf` outside the logger; `LogBus::line` remains only in the three line builders steps 3 and 4 replace.
- The log, step 3: the counters each task writes every 10 s (`[hb] [win] [task] [stk] [can] [panel] [stall] [loop] [hu] [key] [car] [can2] [cap] [ble] [media] [ui] [drive] [net]`) are status lines (`LOGS`): at INF once per «Рядки стану кожні» (`logStatus`, 10 / 60 / 300 s, 60 by default), every window when the tag is at DBG. About six times less text in a pushed log.
- The log, step 4: the head unit's UART is in the CAN recording as bus 2, every frame both ways (records of 8 bytes; `tools/can_dump.py hu FILE...` prints the frames back). In the text log a head-unit command is a DBG line (`D [hu] R cmd=...`) and the per-frame echo TRC only; a key or detent is DBG (`D [key] ...`). The head unit's `0xFF` after each of our frames is its ACK, now counted (`acksIn` in the `[hu]` status) instead of logged as junk. ESP-IDF lines keep their own level (`E [idf] ...`). The sinks filter by level only: the text filters (web-only `[kev]`/`[hur]`, the echo's flash ban, the push's per-command dedup) are gone.
- The log, steps 5 and 6: Плата › «Журнал», one card: the level, the status interval, each sink's level, and single tags at their own level (rows of tag ▾ level ▾, the tags the board has written since it started, `GET /api/logs/tags`). `tools/logscan.py` reads a pushed boot from GitHub (`--gh mo-4580 --last`) or files, by tag (`--tag hu,key`), level (`--level D`), text (`--grep`), or counts lines per tag (`--tags`).
- The head-unit mapping, step 2 of docs/design/hu-mapping.md: a field the head unit gets can come from another signal (× ÷ +), a constant, or be off, without touching the signal itself. On Авто › мапа ГУ the editor starts with «Звідки» (свій сигнал / інший сигнал / константа / вимкнено); the button shows the rule (`← fuel_level`, `= 90`) and the value column what is sent. `GET/POST /api/hu/fields`, saved in NVS (`huFmap`, up to 32 rules); with no rules the bytes on the wire are unchanged.
- WiFi that is "connected" and carries nothing (the head unit's hotspot after its own uplink dropped) is left and joined again: after 3 log pushes in a row with no answer at all (DNS, TCP, TLS; an HTTP refusal counts as an answer), `W [wifi] ... leaving and rejoining`. The next try waits 5 min, then twice as long each time up to an hour, so a hotspot with no internet (the garage) costs a few seconds an hour. Pushes through the app on USB do not count.
- Six gears: the owner's car is a six-speed, and the computed five-speed table read its 6th (431) as N and its 2nd and 3rd at the edge. The defaults are now the ratios the 1.6.1 learner measured on a drive, 147 207 275 359 431 for 2nd..6th (1st, 80, from their progression), and Settings › «Передачі» has a 6th. The table moves to new keys, so a 1.6.1 board takes these defaults once.
- The head-unit map names a field the board fills itself «плата → ГУ» (it is sent over the UART like the rest), not an empty binding, and it can be bound like any other: a CAN ID or a constant feeds it and the computation stands down; «плата → ГУ» in the editor brings it back.
- Everything that goes to GitHub is one card, Плата › Логи в GitHub (the log, the CAN recording, the daily limit); the car page's recording card links to it. The live page's recording is «CSV сигналів у браузер», not a second «Запис».

### Fixed
- The head unit's odometer was empty for a drive: 0x5FD did not come once (1.6.1 `[drive]`, two drives), so the field now takes the cluster's odometer (0x5C5, always on the bus) whenever 0x5FD is silent, not only under the 7D 04 experiments.
- «Без WiFi, коли є USB» turned WiFi off for any client on the USB link, the WebUSB page too, which carries no internet: the board then had neither. It now goes off only for a client that carries the board's internet (MegaOpen Companion).

### Removed
- The live page's «CSV сигналів у браузер» recorder: the board's own CAN recording, `[drive]` in the log and the log push cover it, and it only ran while the tab was open.

## [1.6.1] - 2026-10-26

1.6.0 and everything after its rcs, as one version both the board and MegaOpen Companion see as newer than any 1.6.0 (an rc1 put on by hand took itself for the final).

### Added
- `[drive]` every 10 s while the car runs: speed, rpm, the gear ratio and gear, clutch, neutral, reverse, the three lights, the handbrake and the two reminders' signals, so a whole drive can be read from the pushed log.
- The gear learner: steady speed/rpm ratios with the clutch up, binned; once a minute `[gear] steady ratios seen: …` lists the peaks (one a gear) next to the table in use.
- Settings › «Передачі»: the table (km/h per 1000 rpm x10, 1st..5th), live. The defaults were computed, not measured: copy the learned peaks here.
- The odometers (0x5FD, and the cluster's 0x5C5) come rarely, at ignition and then per km: a board that started or rebooted after that frame showed none for the drive. The last values are kept in NVS (on a change, at most once a minute) and stand in after a boot until the car sends its own; `[drive]` shows the odometer and its age.
- The head-unit map shows «рахує плата» instead of «+ прив'язати» on a field the board computes itself (average speed, trip time, gear, consumption): it needs no CAN binding.
- Авто — наживо › правила: «Додати заводські правила» puts the low fuel, no headlights and handbrake rules into empty rows (switches them on if present) and saves; your own rules stay.

## [1.6.0] - 2026-10-26

The USB link (docs/design/usb-link.md) and the app rebuilt (docs/design/companion-app.md).

### Added
- **The USB link.** The board's USB-C port, plugged into the head unit, is a composite device: the console (CDC) and a vendor interface for MegaOpen Voice. Frames carry HTTP to the board's own web server (every route, the request gate and the settings lock as over WiFi), the panel alerts as events, and pings. `/api/status` `usb`; `tools/usb_link.py` talks to it from a PC (Windows needs no driver on `t2can`).
- **MegaOpen Companion over USB.** Plugging the board in opens the app; the alerts come as events, with no WiFi and no address. «Відкрити сторінку плати» shows the board's own page inside the app, over USB or WiFi alike (the page's calls, downloads and the firmware upload go through `window.MOLink`).
- **The app as the board's internet.** On the link, the board's update check and download and the log push go through the app's network (Android TLS, GitHub over https only, 16 kB credit), over WiFi otherwise. The board composes the requests as before, token and all. Bench: v1.5.4 installed through it, 1.67 MB in 24 s.
- **The settings page in the apps.** MegaOpen Companion carries its own copy of the board's page (the same web/ files, `tools/export_page.py`), shown when the board runs the same firmware or has no page; the WebUSB page (`web/usb/`, Chrome on a PC, GitHub Pages) brings the board's page over USB.
- `FEATURE_WEB_PAGE` (on by default): off, the board serves no settings page, 83 kB less flash; "/" is then a rescue page (`src/net/RescuePage.h`) with a firmware update from a file and the WiFi network, so nobody is locked out.
- **Reminders that keep reminding.** In MegaOpen Companion each alert has a repeat (once, every 15, 30 or 60 s; lights and handbrake every 30 s by default): while its condition lasts the phrase is said again, and an Android notification (the alerts channel, no chime over the voice) stays until it is fixed. The app asks the board `GET /api/signal?name=…` every 2 s.
- Settings › «Нагадування»: the speeds the lights and handbrake reminders start at (10 and 5 km/h by default, each off below half of it; 0 switches one off), live.
- **The whole log reaches GitHub.** What a boot could not push (no internet, a power cut, a crash) is sent after the next boot from the flash log, from the line after the last one pushed, as `boot-N/flash-PP.log`; a boot that pushed nothing is sent whole. The first boot on this version sends the earlier boots still on flash once.
- **The crash itself.** After a panic or a watchdog the whole coredump is kept on flash (`/crash/`) and pushed as `boot-N/coredump.bin` (N: the boot that crashed); `esp-coredump info_corefile -t raw -c coredump.bin firmware.elf` shows every task's stack.
- The head unit's UART in the pushed log: each frame kind (direction, command and sub-command) the first time and again whenever its bytes change; the board's own telemetry to it at most once in 10 s per kind (its values are in `[drive]`). On 1.6.0-rc5 the key was the command alone, so every 7D frame went through: 90 % of the pushed log, 2.3 MB in 39 minutes.
- Авто › «Діагностика клімату» › «Перевірити всі варіанти»: one pass to the climate ECU (0x744), ~25 s, each step logged word for word with the bus around it (frames and distinct ids a second; a cluster that reboots drops its ids): the reads DDT lists, `21 80`, `21 C0`, `21 C1`, `21 C2`, `21 90`, `21 A0`, without a session; `10 C0` (extended session); the same reads in it with `3E 01` between; `10 81` back to the default session, always last. `21 90` (Clim_X84_SERIE's "trame déportée") is decoded in the log: the driver's setpoint, blower, distribution, recirculation, A/C, a hypothesis until our ECU answers.
- «Опитування в сесії 10 C0» (`diag.session`, off by default): the polling read opens `10 C0` first, keeps it with `3E 01` every 2 s and closes it with `10 81` when switched off; a refused session leaves polling without one.
- The allowlist (`src/diag/ClimDiag.h`) takes those nine requests to 0x744 and nothing else: no write, clear, routine, reset or IO control.
- **The car's CAN to GitHub** (Плата › Логи в GitHub › «Запис CAN авто», off by default): on, the board records the car bus (change-only) and sends each closed recording file in 512 kB parts as `cap/<session>/<file>-<part>.bin`, at most «МБ на день» (200 by default). `tools/cap_fetch.py` fetches and joins them for `tools/can_dump.py`.
- The board's date: MegaOpen Companion sends its clock with every alerts poll (`t`) and in the USB link's HELLO; the board sets its clock when it has none or is 5 s off (`[time]` line). The head unit sends hours and minutes only.
- Settings › «Без WiFi, коли є USB» (`wifiAuto`, off by default): WiFi off once the app has been on USB for 10 s, back 30 s after it is gone. 27 kB more heap while off.

### Changed
- The head unit's «Unknown Contact» (a Bluetooth source with no name, seen as «Unknown ContactE» on a DUDU) is not taken as a title or artist: the last real one stays on the glass.
- MegaOpen Voice is now **MegaOpen Companion** (MegaOpen Компаньйон), rebuilt as a product (docs/design/companion-app.md): Kotlin coroutines, Jetpack Compose screens (Home, Voice, Board, More), settings in DataStore, a foreground service of type connectedDevice, USB and WiFi behind one `Board` interface (the board's page in the app works over either), the repo and allowed hosts in one place in the build, unit tests and lint in CI. New package `ua.megaopen.companion`: an installed MegaOpen Voice 1.5.4 is a different app, uninstall it once. The release APK is `megaopen-companion-X.Y.Z.apk`.
- The T-2CAN builds use TinyUSB (`ARDUINO_USB_MODE=0`) with the board's own USB PID (`variants/t2can`, 303A:80C3) and a composite device descriptor. About 23 kB of DRAM at boot.
- The web server's sockets have TCP_NODELAY: a small reply no longer waits for a delayed ACK (over the link 250 -> 7 ms).

### Fixed
- The car rebooted 12 times in a row (1.6.0-rc5, the coredumps): the log push's TLS handshakes, a backlog sent every 3 s, kept IDLE on core 0 out for 5 s and the task watchdog fired; each next boot's backlog did it again. The push runs at IDLE's priority now (they take turns) and a backlog goes every 6 s.
- A coredump carried the GitHub token and could carry the WiFi passwords (it is the board's RAM): they are overwritten before it is pushed, with any other `github_pat_` / `ghp_` text. Revoke a token that went out with a 1.6.0-rc dump.
- MegaOpen Companion put on by hand (not through its own «Оновити») took itself for the final 1.6.0 and saw no newer rc («This is the newest» on an rc1 install). Every release APK now carries its tag (`BuildConfig.RELEASE_TAG`, from release.yml), used when no install tag is remembered.
- A firmware upload through the page (or the app's page over USB) could reboot the board on the task watchdog: httpd wrote the upload as fast as it came, each 64 kB erase stopping the cache, and IDLE on core 0 starved for 5 s. Found by the new coredump upload (bench boot 76: `httpd` → ElegantOTA → `Update.write` → `esp_partition_erase_range`), the same `task_wdt` in `httpd` as the car's on 1.6.0-rc1. A tick after every 8 kB chunk was not enough (the bench rebooted again, in `wifi`: the whole core is busy receiving): core 0's idle watchdog is off while a page upload runs, back on at its end or 60 s after its last chunk. The GitHub install was never affected.
- With Bluetooth off, `/api/ble` (the page's Overview) asked NimBLE for its bonds and the board rebooted on an assert in the stack's lock (the car on 1.5.3, `httpd`, `getNumBonds`). It now answers «no bond» while the stack is down.

## [1.5.4] - 2026-10-25

### Added
- **MegaOpen Voice** (`megaopen-voice-1.5.4.apk`, source in `android/`): an Android app for the head unit that says the panel's alerts out loud over the music (the music ducks, then comes back). No root, Android 7+. It finds the board itself (mDNS, then the hotspot's addresses; or type the IP), polls `/api/alerts` every 1.5 s, says each new alert once (not what came before it was listening) and starts with the head unit. Settings: on/off, Ukrainian, English or the system's voice, volume, per alert on/off and your own phrase (`{v}` for the value), other alerts with the board's text, and «Тестове попередження з плати» for the whole chain. It updates itself from the same GitHub releases as the board (Оновлення › «Перевірити», «Тестові версії (rc)»; Android asks to confirm each install). Every release APK is signed with one key, kept in the repository secrets; release.yml refuses to publish an APK signed with any other.
- For the voice app (Android, `android/`): `GET /api/alerts?since=N`, the panel alerts shown since number N (the last 16; `last`, `upMs` for a reboot), and `POST /api/alerts/test {sig}` for a test entry; the board announces itself as `_megaopen._tcp` over mDNS so the app finds it on the head unit's hotspot.
- Panel reminders: «Lights off!» when the car moves at 10 km/h or more with neither the dipped nor the main beam on, «Handbrake on!» at 5 km/h or more with the handbrake up. Two derived yes/no signals (`drive_no_lights`, `drive_handbrake`, each with a speed band so a crawl round the threshold does not flicker) and two factory alert rules on their rising edge. A board whose alert table is still the factory one gets them at once; a saved table keeps its own rules.
- The cabin temperature, read from the climate ECU: Авто › «Діагностика клімату», **off by default**. The board's first transmit on the car bus, for one request only: `21 C1` to the climate ECU (`0x744` → `0x764`, KWP2000 over ISO-TP), and the flow control `30 00 00` its multi-frame reply needs. No diagnostic session is opened (PyRen and DDT4all send `10 C0` first; the owner saw the cluster reboot when a tool opened diagnostics), no tester present, no other ECU, no write. «Спробувати один раз» sends one request and logs the reply (`[diag] climate 21C1: cabin … C`); «Читати салон» polls every 2-60 s (5 by default). New signals `cabin_temp` and `clim_sun` on «Авто — наживо». The controller goes to normal mode only on a locked bus rate and while the read is on, never on a quiet bus; a transmit error count past 96, error passive or bus off puts it straight back to listen-only and latches off (`W [can2] diagnostic read stopped`). A negative reply stops the polling; three timeouts back off for a minute. `/api/diag`; `tools/check_rules.py` fails on any other transmit path to CAN A. `CLAUDE.md`'s CAN A rule says so.
- Average speed and trip time for the head unit, which the car does not broadcast (its cluster computes them): the board's own, from the distance it already integrates for the fuel figures and the time with the engine on since boot. `avg_speed` (from 1 km) goes in `0x7D 03` D3-D4 and `0x21` D0-D1, `trip_elapsed` (minutes) in `0x21` D2-D3. The tablet showed 299 km/h with nothing sent.
- Signal `intake_temp`, the intake air: `0x061` (ECM_to_GCU) byte 8 − 40 °C, from the X84 broadcast matrix (v84.05), a hypothesis until checked in the car. A saved map shows it after «Заводські прив'язки».

### Changed
- «Експерименти ГУ» › «бак % у 7D04» (was «пальне в 7D04»): the tank in per cent ×10 (litres ÷ 60) in `0x7D 04` D4-D5, where DUDU draws «518 km ▬ 84%» in its status bar; the litres it sent read as a per cent there. Every canbox's `F2 08` is 229 %, which is why nobody sees it.
- `service_km` (`0x715` b5): 150 km a step, not DDT's 250. 184 × 150 = 27 600 against the owner's cluster 27 654; ×250 gave 46 000. Still a hypothesis.
- Lights › «Follow dash»: «Brightness» is the ceiling. The dash at full with «Dash gain» 100 % lights the rings at «Brightness», a dimmed dash proportionally less (brightness × min(dash × gain, 254) / 254); the gain can lift a dim dash up to it, never past. 1.5.3 put the dash level itself on the rings, ignoring «Brightness».

## [1.5.3] - 2026-10-25

### Added
- Panel menu: the board's address at the root, before «Power off» (`IP 192.168.234.46`, or `AP MegaOpen-XXXX` on its own hotspot). Choosing it closes the menu and shows the same full-size notice as on connect, with the mDNS name. It is no longer only once per connection.
- The settings file (Плата › «Налаштування у файл») carries the CAN map too: `decode`, the table exactly as the board keeps it (bindings, constants, the ✓ checks; base64), and `hu`, what goes to the head unit (send, units, the experiments). Loading a file checks both with the rest before anything changes, and the preview says the map will be replaced. An older file without them leaves the map alone.
- The last crash in the log: at boot, a coredump on flash becomes one `W [crash]` line (task, pc, cause, the image's ELF sha, the backtrace) and is erased. It reaches the flash log and the log push. The release run keeps each build's ELF as an artifact (`elf-vX.Y.Z-rcN`, 90 days) to decode it.
- Lights › «Follow dash» and «Dash gain» (menu and web): the knob rings dim with the instrument illumination off the car's CAN (`0x645` b1, the dash rheostat), times the gain (10-250 %). With the car's lights off (`0xFF`) or no fresh value, «Brightness» as before. Off by default; stored in the knob blob's two spare bytes, so older settings load unchanged.

### Changed
- `0x645` b1 (instrument illumination) is no longer a hypothesis: the owner saw it follow the rheostat in the car.
- 26 more factory rows are measured, as the owner ✓'d them on «Авто — наживо» in the car: speed, rpm, steering, odo_cluster, out_temp, eng_temp, handbrake, reverse, brake, rear_defrost, rear_defrost_req, wipers, fog_front, fog_rear, fuel_level, ac_request, ac_comp_upc, clim_ac_sel, oil_level, clim_fan_state, batt_v, clutch, neutral, reverse_upc, locked, belt_drv. «Заводські прив'язки» no longer turns them back into hypotheses.
- Panel menu › Lights › Brightness follows the knob: each step is applied (and stored) at once, not on the press that ends the edit. Settings marked `F_LIVE` in the registry do this.

### Fixed
- Log push: one file a minute, as meant. The lines logged during a push (a few kB) counted as a backlog, so the next file went 3 s later: a file every ~8 s, four times the commits. Only a backlog of 16 kB or more (a spool filled offline) drains fast now.
- Head unit, «t° ОР» experiment (`0x7D 09`): the tablet showed 199 at 95 °C. Its "oil temp" is (78 − D2) mod 256, not D2 − 40 (the owner's sweep: D2 0 → 78, 40 → 38, 78 → 0, 160 → 174); the coolant now goes out as D2 = 78 − T and reads right. Below 0 °C it shows 0.

## [1.5.2] - 2026-10-25

### Added
- The log to a private GitHub repository, for reading a drive from home: Плата › «Логи в GitHub». On a network with internet (the head unit's hotspot) the board puts the new lines into the repo as a file once a minute (`mo-XXXX/boot-NNNNN/PPPP.log`); without internet they wait in a 1 MB PSRAM spool (about an hour and a half) and go once the link is back. On by default: paste a fine-grained token (Contents read/write on that one repository) and it starts. The token is never shown back. `/api/logpush`, `[push]` lines.
- Плата › Оновлення: «Тестові версії (rc)». On, «Перевірити» takes the newest release on GitHub, an rc included (`/releases`, the ten newest); off (the default), the latest final as before. The board remembers which tag it installed from (NVS `fw/tag`), since an rc's image carries its final's version: rc2 is offered over rc1, and the final over its rc. `POST /api/fw/channel {pre}`, `/api/fw` `pre`.

### Changed
- AffaDisplay v1.4.1 (was v1.3.0): its geometry now says what the glass shows of the menu (rows 20, header 26) and records the info rows' 8; MegaOpen's own widths are checked against it at build time.
- One TLS job at a time: a GitHub update check refuses with `tls_busy` for the few seconds a log push holds the connection.

### Removed
- The glass-sweep route `/api/display/inforaw` and its steps 7-15 in Сервіс › Заміри скла: the info row showed 8 at every declared length, so 8 is the width.

### Fixed
- Panel menu: an item row is laid on the 20 cells the big font shows, not 26. A setting's value and a submenu's `>` stood past the glass's edge (Lights › Brightness showed no value). The header keeps 26. The media screen's menu layout scrolls title and artist in 20 columns too (it lost the last 2). Measured with Сервіс › Заміри скла: menu rows 20, header 26, fullscreen 22 for any glyph.

## [1.5.1] - 2026-10-21

### Changed
- Panel bus watch: with «Реєстрація функцій» off (passive, the OEM radio owns the handshake) MegaOpen draws over the radio again. The watch only reports it (health `panel_beside_radio`, state ok, one `[panel]` line) and the TX gate is open from the start (no 3 s listen). Going silent beside another radio is now only for FuncsReg on, where our registration would fight it.
- A failed GitHub update (`/api/fw` `err`) and a recording error (`/api/cap` `error`) are codes too (`gh_rate`, `gh_http 503`, `cap_full`…); the detail stays in the log in English. USER-GUIDE's health table shows the page's Ukrainian words for each code.

## [1.5.0] - 2026-10-21

### Added
- Web page in English: the EN / UA switch in the header (per browser; `?lang=en` in the address too). Words live in `web/i18n/uk.json` and `en.json`; the English file is fetched only by a viewer who picks it. Every screen.
- CI job `smoke`: `tools/rc_smoke.sh --sim` against the web simulator on every push (the checks only a board can pass are skipped by name). `rc_smoke.sh` checks codes, not wording.
- CI fails on Ukrainian text left in `web/js` or `index.html` outside the dictionaries (`check_i18n --strict`).
- The board's address on the panel, once per WiFi connection: `IP 192.168.x.y` / `megaopen.local` at home, `AP MegaOpen-XXXX` / `192.168.4.1` on its own access point, as a 6 s fullscreen that waits for the menu and a held glass. A link back within 30 s with the same mode and address is not announced again.
- GitHub update: each tenth of the download logs its lowest internal heap, its largest block and how long it took; `/api/fw` adds `stepMin` and `stepMs`.

### Changed
- `/api/health` chains carry only `code` (no English `why`): the page has the words in both languages. Огляд no longer tells you to turn the knobs or watch the rings.
- The catalog's names (settings, their choices, signals, Raise rows, layouts, styles, scenes, console) are in the page's dictionaries, so they follow the language switch. `/api/catalog` sends ids: no setting `label`, a `name` only where the id is not a word (`AF-RDS`, `125k`), enum lists name their kind (`dom`), Raise rows a section id and an English unit; `/api/map?meta=1` leaves the label slot empty. No Ukrainian text is left in the firmware outside comments. Signal lists show a name for every signal the Raise list names (they showed its id, `speed`), and doors and radar sensors say which ones.
- `/api/health` `keys.dest`: a destination that cannot take a key says why with a `code` (`uart_off`, `hu_silent`, `no_ams_role`…) in place of an English `why`.
- Error replies are codes, one refusal a line (`pulseMs: number`, `param exp`, `busy`), not English sentences; the page shows them in its language. A refused macro now marks its row again (the reply said `row 3: …`, the page looked for `3: …`).

### Fixed
- Плата › Усі налаштування: a number setting showed its unit twice ("Брязкіт, мс, ms"), and the canbox version (text) was a number box.

## [1.4.0] - 2026-10-21

### Added
- `tools/car_report.sh <board>`: after a drive, one folder and `.tar.gz` with status, health, settings, the settings file, the live map, the logs (ring, this boot, the boot before), the update state and the capture list, plus `summary.txt`. GETs only.
- «Авто — наживо»: «✓ підтвердити» / «↺ скасувати» on a hypothesis row. A confirmed signal loses its «?» everywhere (menu, car screen, alerts, the G-meter ring); «Заводські прив'язки» brings the hypotheses back. `/api/map` `{confirm:{sig,on}}`.
- «Авто — наживо»: a recorder for the car test (record/pause, markers, CSV), with no extra requests to the board.
- RAV4 page: the bonnet goes to the head unit (`0x24` D1 b7). The X84 broadcast has no bonnet contact, so it has no factory binding.

### Changed
- `/api/map`: the map part of a request is all or nothing. A refused confirm or a table that would overflow leaves the saved map as it was; «↺» restores each row's factory flag.
- RAV4 page: the battery row shows the live volts and says protocol 112 has no field for them, with a link to the head-unit battery experiment.

### Fixed
- `/api/health` `clock`: the head unit's clock (`0x9D`, once a minute) counts as a source. It used to say `clock_no_phone` while the head unit kept the panel's clock right. New codes `clock_phone`, `clock_hu`, `clock_no_source` (replace `clock_ok`, `clock_no_phone`), and `src`. With no phone the pane's clock face runs from the head unit's time (it stayed blank).
- CI: the phone-width audit no longer hangs on the live page (the recorder's leave-page warning is attached only while a recording is unsaved); a hung page now fails by name.

## [1.3.0] - 2026-10-21

### Added
- Pane scene G-метр: the felt force from 0x2A0 (left turn → right, braking → up) as a dot with a trail, a 0.5 g ring and a 3 s peak; the ring dotted while the decode is unverified, a still dotted ring with no fresh data. Not in the carousel.
- Car screen preset «Поїздка»: consumption now/average, range, trip distance (`--` with no data; the trip keeps its last value with `?` after the engine stops); Display › Car screen › Preset, `carPreset`, and a card on Екран.
- Head unit experiment: battery volts on the tablet (off by default). The RAV4 profile has no battery field, so the volts ×10 go in a borrowed one: the range of `0x21`, `0x7D 04` D4-D5, or the current value of `0x23`. A field another experiment uses is refused (400, in Ukrainian on the page).
- `/api/health` `hu.climateChanges`: changed `0x28` frames since boot, to compare with climate pop-ups seen in the car.
- CI job `ui`: the web app audited at phone width (390 and 360 px, light and dark) on every push. It fails on a page wider than the screen, text under 4.5:1 contrast, a control under 40 px, or a console error; findings show as annotations on the run page.

### Changed
- Panel menu: the car screen settings moved into Display › Car screen.
- AffaDisplay v1.3.0 (from v1.1.1). The dev-only `/api/display/inforaw` (glass sweep) sends through the library's `showInfoRow`: the same bytes as before, and the hand-built frame (`display/InfoRaw.h`) is gone. `rows=3` puts the ruler in all three info rows (sweep step 15).
- CI and release workflow actions on their current major versions (Node 24).

### Fixed
- AffaDisplay v1.1.2: urgent panel jobs (power on/off) go out in the order they were queued; two of them used to go out reversed.
- Head unit: the tablet's climate overlay no longer pops up by itself. The outside temperature in `0x28` changes only after a new reading has held for 60 s (a flickering degree changed the frame every 5 s), and the 5-byte A/C form goes out only from a measured or pinned signal, not a guessed one.
- Кнопки: the macro card fits a 360 px phone (it overflowed by 13 px with Linux fonts).

## [1.2.0] - 2026-10-21

### Added
- Settings to a file and back: GET/POST `/api/config` and a card on Плата; a dry run shows what changes before anything applies. Passwords never go in the file.
- SK6812 RGBW LED strips: a third strip type, white from min(r, g, b).
- Panel bus watch: the panel link listens 3 s from the first poll before it sends; any other radio's sync keeps MegaOpen silent (health `panel_foreign_master`), and it re-detects 30 s after the syncs stop. A lone cluster reads `panel_cluster_only`. First registration comes ~3 s later than before.
- Health: `hu_dialect_vw` when the head unit speaks the VW canbox profile instead of Toyota, with the 0xF3 "not supported" count.
- Glass sweeps: a 10-minute camera procedure (`docs/glass-sweeps.md`) with a step-through on Сервіс; a developer-only `/api/display/inforaw` row with a declared length.
- `SECURITY.md`: what is exposed and how to report; README and the user guide say to change the AP password at first boot.
- `docs/RELEASING.md`: how a release goes.
- `tools/rc_smoke.sh`: a release-candidate smoke test against a running board (reads and dry runs only).

### Changed
- Requests from other sites are refused: every change needs the header `X-MegaOpen: 1` and the board answers only to its IP or its own `.local` name. `/update` leads to the Плата page; a WiFi scan is a POST.
- POST `/api/settings` lists every refusal in its 400 and applies nothing; `?dry=1` checks without applying.
- Phone layout: 40 px touch targets, whole select labels below 560 px, 4.5:1 contrast in both themes.
- Errors and reasons on the page in Ukrainian.
- The page polls only what is on screen; Огляд every 2 s.
- The 32 kB log ring and request-only buffers live in PSRAM; idle internal heap ~99 kB (was ~58).
- AffaDisplay v1.1.1 (docs and examples only; same firmware).
- README: flashing and building from a clean machine (esptool 5 spelling, download mode, checksums, Windows path limit).

### Fixed
- `t2can_usbhid` builds on Windows again (the compile line passed the 32 kB limit).
- A web key sent while four others wait for the panel's verdict is logged as unwatched; before, its refusal went unlogged.

## [1.1.0] - 2026-10-21

### Added
- Firmware updates from GitHub Releases on the Плата page (T-2CAN): check, download the env's image, verify its SHA-256 against `SHA256SUMS`, reboot into the OTA trial from the page or once the car stands. Never automatic, never while driving.
- Media styles tape (a cassette with turning reels) and typewriter; pane scenes tape and tunnel.
- Volume keys fall back to the phone while the head unit is silent (not in the first minute after boot).
- Direct tier: web screens for the 3x3 key matrix and the KEY1/KEY2 ladder.
- Head unit: a PRND probe on 0x7D 06 behind a switch, with a web sweep.
- `uartBaud` in the settings registry: a list of canbox rates, applied at reboot; a stored rate outside the list can be repaired.
- User guide (`docs/USER-GUIDE.md`): first boot, the panel menu, live car page and alerts, media, keys, updates, health codes.

### Changed
- AffaDisplay v1.1.0: its fullscreen sends the OEM geometry; MegaOpen's own fullscreen sender is gone.
- Car screen and the media Fullscreen layout use the OEM fullscreen geometry (row 1 no longer indented, row 3 gets room).
- Media rows and the header fit the glass's 22 columns.
- Key routing knows what each sink can do; the AMS default no longer takes the phone class.
- AMS volume: commands per press measured from the phone's volume instead of a fixed 15.
- No automatic screen covers a live popup, in any layout.
- The generated web page is no longer tracked, so a build cannot mark the tree dirty.

### Fixed
- Steering angle centred on 0x7FFF, as the X84 matrix says.
- A Renault 2-bit state's invalid value no longer reads as "yes"; belts and the passenger seat read 1 = yes.
- AMS reads the whole of a long title, artist or album; a failed or short read never replaces the shown title.
- BLE: AMS long reads and ANCS attribute requests wait for idle keys; the AMS caches and bonds are cleared only on the BLE task.
- Panel menu: a Bool pressed twice toggles twice; a knob edit from the menu is no longer lost to a web save.
- Car screen: a "none" row stays blank in its place instead of moving the rows below it up.
- Simulator: GET /api/settings no longer fails on a text setting.

## [1.0.0]

### Added
- LilyGO T-2CAN firmware: AFFA/Carminat panel on CAN B, car on CAN A (listen-only), Raise/RAV4 canbox to the head unit over UART.
- iPhone over BLE: now playing (AMS), notifications (ANCS), clock (CTS).
- Two rotary knobs with WS2812 rings on QWIIC.
- Car CAN decode for the X84 broadcast rows, with gear, consumption and range derived.
- Live car page (`#live`) with per-signal age and verified state.
- Panel alert rules: a popup on the display when a signal crosses a threshold.
- Car screen: three CAN signals on the panel instead of now-playing.
- Panel menu: hold the right knob to browse settings and car data on the display.
- Settings registry: every setting described once, shared by the web app, the panel menu and `/api/settings`.
- Head-unit experiments behind switches.
- OTA trial: a new image confirms itself after 60 s healthy and rolls back otherwise.
- `env:direct`: ESP32 DevKit V1 without a panel (key matrix + resistor ladder).
- CI on every push and pull request; tagged releases with OTA and factory images per env.

### Changed
- The panel link is the AffaDisplay library's (`affa::TwaiLink`); renders go through it from any task.
- Capture settings, log level, key routing, head-unit message switches and Raise tuning moved into `/api/settings`; their own routes are gone.
- Panel alert rules moved from `/api/map` to `/api/settings`.

### Removed
- DevKit + board V setup, the runtime CAN role and the bench CAN diagnostics.
- CloudLink and `cloud/`.
- OTA password; the WiFi password protects the board.

[Unreleased]: https://github.com/Barbedx/MegaOpen/compare/v1.2.0...HEAD
[1.2.0]: https://github.com/Barbedx/MegaOpen/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/Barbedx/MegaOpen/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/Barbedx/MegaOpen/releases/tag/v1.0.0
