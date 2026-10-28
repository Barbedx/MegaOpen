# The «Мапа» page: signals, head-unit fields and alerts in one place

Status: **planned, owner approved (2026-10-28), option A (tabs)**. Step 4, second
half, of `docs/design/hu-mapping.md`. Web only: no firmware change, no API change.

This file is the whole brief for the agent that builds it. Read it, then
`CLAUDE.md`, then the files named below. Nothing else from earlier sessions is
needed.

## Why

Since 1.6.5 the two layers of the head-unit mapping are edited in two places:

| layer | where today | file |
| --- | --- | --- |
| where a **car signal** comes from (CAN row, constant, off) | page «Авто — наживо» (`#live`, a sub-page opened from a card on «Авто») | `web/js/72-live.js`, `SCR.live` |
| what the **head unit** gets for each field («Звідки»: own signal / another signal ×÷+ / constant / off), test send | card «RAV4 / мапа ГУ» on «Авто» (`#car`) | `web/js/70-car.js`, `SCR.car` (`mapRow`, `mapEd`, `srcBtn`, `vCell`, `raiseRows`, `withRules`, `map()`) |

The chain «CAN row → signal → head-unit field» is split across two pages: the
field editor's «змінити в таблиці сигналів» jumps to another page through a
global (`LIVE_OPEN`). The owner wants one page.

## What to build

A new sub-page **«Мапа»** (`SCR.map`, hash `#map`), opened from a card on
«Авто» in place of today's «Наживо» card. Three tabs at its top, a `seg()` of
the kit (`web/js/20-kit.js`):

**Сигнали | Поля ГУ | Сповіщення**

The tab is part of the hash: `#map` (= signals), `#map:fields`, `#map:alerts`.
Switching tabs rewrites the hash with `history.replaceState` (no `hashchange`,
so no re-render of the whole page) and shows/hides the three panels; all three
are rendered once in `render()`.

### Tab «Сигнали» (all of today's signals card from `72-live.js`)

Unchanged behaviour, moved:
- «Оновлювати» (auto, every 2 s) and «Лише з даними» switches, the alert stats row;
- «Заводські прив'язки» (`POST /api/map {reset:true}` after `ask()`);
- the table: signal (name + id), value, age, source chip = the button that
  opens the binding editor (`bindEd`/`BIND` in `70-car.js`, `GET /api/map?sig=`),
  ✓ / «✓ підтвердити» / «↺ скасувати», 🔔 (adds an alert rule → now switches to
  the «Сповіщення» tab and scrolls to the new row);
- the `#idl` datalist for the CAN id field.

New:
- **«→ поле ГУ» marks.** In each signal row, a small muted line listing the
  head-unit fields that take this signal: a field whose effective rule is its
  own signal (rule null and def null, or `from:"own"`) takes the signal of its
  own id; a field with `from:"sig"` takes `src`. Data: `GET /api/hu/fields`
  (`rows[].{id, rule, def}`), field names from `CAT.raise` (`name`) / `k.raise.<id>`.
  Clicking a mark switches to «Поля ГУ» and scrolls to that field's row,
  highlighted for 2 s (the `.hl` class exists).

### Tab «Поля ГУ» (today's RAV4 card from `70-car.js`)

Unchanged behaviour, moved:
- «Протокол ГУ», «Слати в ГУ», units (fuel/trip), the group filter, the field
  table with «Звідки» editor (`mapEd`), test send ▶ (`POST /api/map/test`);
- «Прогін селектора» (the `0x7D 06` sweep): keep it as a row of this tab,
  directly above the table.

Changed:
- «змінити в таблиці сигналів» under «свій сигнал» in the field editor: no
  page jump and no `LIVE_OPEN` global any more. It switches to «Сигнали»,
  turns «Лише з даними» off, scrolls to that signal and opens its binding
  editor there (`SCR.map.bind(i, id)`, today's `SCR.live.bind`).
- In «Звідки» → «інший сигнал», the chosen signal's name is a link that does
  the same.

### Tab «Сповіщення» (today's alerts card from `72-live.js`)

Unchanged: the 16-row rule table (`drawAlerts`/`readAlerts`), «Зберегти»,
«Додати заводські правила», the help row.

### «Авто» after the move

`SCR.car` keeps the gears card, CAN A, climate diag, the CAN recording and its
files, the head-unit UART log fold and the ID table fold. Its first card,
«Наживо» → «Відкрити», becomes **«Мапа»** → «Відкрити» (`href="#map"`), with a
sub-line naming the three tabs. The RAV4 card and all its code leave `70-car.js`.

### Polling (one request per tick, as now)

`99-main.js` calls `cur.tick()` every 1 s. In `SCR.map.tick()`:
- «Сигнали» visible and auto on: `GET /api/map?live=1` every 2nd tick (as `SCR.live.tick`);
- «Поля ГУ» visible: today's `SCR.car.map()` (`/api/map` + `/api/hu/fields`) every 2nd tick;
- «Сповіщення»: nothing.
`/api/hu/fields` for the «→ поле ГУ» marks: fetched in `render()` and again
after any save that changes a field rule; not every tick.
Keep the existing rule that a redraw never runs while an editor or an `ask()`
is open in that table (`draw()` returns early on `.ask` / `.bindEd`).

## Files

| file | change |
| --- | --- |
| `web/js/72-live.js` | becomes `web/js/72-map.js`: `SCR.map` with the three tabs; `SCR.live` is deleted. The JS files are concatenated in sorted name order (`tools/gen_pages.py`): the shared helpers it needs from `70-car.js` (`bindEd`, `BIND`, `bindLbl`, `fmtV`, `mapRow`, `mapEd`, `srcBtn`, `vCell`, `ruleLbl`, `raiseRows`) are top-level consts there and stay defined before it. Move the field-map ones (`mapRow`…`raiseRows`, `withRules`, `map()`, the sweep) into `72-map.js` if nothing on «Авто» uses them any more; leave `bindEd`/`BIND` where both can see them. |
| `web/js/70-car.js` | the RAV4 card and its handlers go; the «Наживо» card becomes «Мапа». `tick()` stops calling `this.map()` (and the `#cMap` guard goes). |
| `web/js/99-main.js` | `route()`: `const id=(location.hash||'#home').slice(1).split(':')[0]` so `#map:fields` lands on `SCR.map`. Nothing else. |
| `web/i18n/uk.json`, `en.json` | new keys: the page title, the three tab names, «→ поле ГУ» mark text, the «Мапа» card on «Авто». Reuse every `lv.*` / `ca.*` key that moves. `tools/check_i18n.py --strict` (CI) fails on a key no file uses: delete the ones that lose their last user (e.g. `ca.live`, `ca.liveRow`, `ca.liveSub`, `lv.title` if renamed). |
| `web/sim/mock.js` | nothing expected (no API change); check the page in the sim. |
| `tools/ui_audit.mjs` | `PAGES`: replace `'live'` with `'map'`, `'map:fields'`, `'map:alerts'`. |
| `docs/USER-GUIDE.md` | «The live car page» section → «The map page» (three tabs; where a signal comes from vs what the head unit gets; alerts). «After an update»: «Заводські прив'язки» is on «Мапа» › «Сигнали». |
| `CLAUDE.md` | the symptom table: `live page #live` → `«Мапа» page #map`; «press «Заводські прив'язки» on the signals page» → «on «Мапа» › Сигнали». |
| `docs/design/hu-mapping.md` | under slice 4, an *As built* line pointing here. |
| `CHANGELOG.md` | `## [Unreleased]` › `### Changed`: one entry. |

Do not touch firmware (`src/`), the API, the catalog, or `web/usb/`.

## Rules (from CLAUDE.md and the owner)

- Work in your own worktree, never in `C:\Users\Andru\source\repos\MegaOpen`
  (other sessions build from it; even `git diff` there has left a stale lock):
  `git -C C:\Users\Andru\source\repos\MegaOpen worktree add ..\MegaOpen-map origin/master`.
- Commit as the repo-local identity (`git log -1 --format='%ae %ce'` must say
  `www.andruxa@gmail.com`). Plain `git commit` (autocrlf as configured); check
  `git ls-files --eol` shows `i/lf` for what you add.
- Push to master: `git fetch origin master && git merge-base --is-ancestor origin/master HEAD && git push origin HEAD:master`. Never force-push.
- One place per concern: no copy of a helper "for now", no compatibility alias
  for `#live`; update every reference instead (`grep -rn "#live\|SCR.live\|LIVE_OPEN"`).
- Match the surrounding code: dense one-line JS, comments in Ukrainian in `web/js`,
  words only through `t()` / the dictionaries.
- No release tag for this alone; it rides in the next release.

## Checks before pushing (all must pass)

```
py -3 tools/check_i18n.py --strict
py -3 tools/check_pages.py
py -3 tools/check_rules.py
py -3 tools/build_sim.py            # needs build/catalog.json: ~/.platformio/penv/Scripts/pio.exe test -e native -f test_catalog
node tools/ui_audit.mjs build/ui-sim.html
~/.platformio/penv/Scripts/pio.exe run -e t2can      # the page is built into the image
```

Then in the sim (`build/ui-sim.html?env=t2can#map`, open in Chrome, or drive
it with `playwright-core` installed in your scratchpad, `channel:'chrome'`):
1. `#map` opens on «Сигнали»; the tabs switch without a reload; the hash follows;
   `#map:fields` and `#map:alerts` open on their tab directly.
2. A source chip opens the binding editor; save as a constant; reopen: the constant is there.
3. A signal row shows «→ <field>» for a field that takes it; clicking it lands
   on that field in «Поля ГУ», highlighted.
4. In «Поля ГУ», a field's «Звідки» → «змінити в таблиці сигналів» lands on that
   signal in «Сигнали» with its editor open.
5. 🔔 on a signal adds a rule and shows it in «Сповіщення».
6. No console errors; «Авто» still shows gears, CAN A, recording, files, the folds.

On the bench board (T-2CAN at `http://192.168.100.110`, OTA:
`H=$(md5sum firmware.bin|cut -d' ' -f1); curl -H "X-MegaOpen: 1" "$B/ota/start?mode=fr&hash=$H"; curl -H "X-MegaOpen: 1" -F "file=@firmware.bin;filename=firmware.bin" "$B/ota/upload"`,
then wait for `[ota] CONFIRMED` in `/api/logs` before anything else; a reboot
inside the 60 s trial rolls back): open `http://192.168.100.110/#map` and
repeat 1–5. Restore any binding you changed there to what it was (the bench's
factory rows matter: read a row with `GET /api/map?sig=<name>` before you
change it). Then `bash tools/rc_smoke.sh http://192.168.100.110`: only the
`-dirty` build-stamp line may fail.

## Done when

- `#live`, `SCR.live` and `LIVE_OPEN` are gone; «Мапа» has the three tabs and
  everything listed above works in the sim and on the bench;
- every check above passes and CI is green on the pushed commit;
- the docs and the changelog say it;
- the report to the owner says what moved where, what was checked where, and
  anything that did not work.
