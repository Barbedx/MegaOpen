#!/usr/bin/env python3
"""The concurrency rules of docs/design/event-tasks.md, as greps.

Every rule here is one a reviewer cannot see from a diff: a call that is fine
on one task and a stuck key on another. So they are checked, not remembered.

    python tools/check_rules.py        # exit 1 and a list when a rule breaks

Rules, by where they bite:

  task files     (src/app/KeysTask.cpp, HuTask.cpp, KeyBus.cpp, the BLE proxy,
                 and from Step 3 the panel task's PanelOwner.cpp, UiPolicy.cpp
                 and diag/CanBench.cpp)
                 - no portMAX_DELAY : a hot-path task never waits for ever.
  the panel      (Step 3) the library object is reached through App::Panel::lib()
                 only from the panel task's files: PanelOwner.cpp, StockRadio.cpp
                 (the extension role's keys, stepped from loop), LoopTask.cpp
                 (the loop) and main.cpp (setup, never a keys or hu function). Every other task
                 renders through MegaOpen::PanelOps, which the library makes
                 safe from any task. No g_display / g_carminat globals.
  HuTask.cpp     - no readBytes() and no bare read(): the HAL UART mutex is
                   shared with the key writes, and both of those can wait on it
                   (§3.3). read(buf, n) after available() is the only form.
  the glue       (main.cpp and the app/ files it moved to: HuLink, KeyPath,
                 PanelKeys, RuleActions, LoopTask) functions that run on the car task (carTap, performAction,
                 onSignalsRenamed) never take the settings lock: a copy or
                 tryLock (Cfg::Config). Functions that run on the keys or hu task must not write the
                 render state (App::Ui's since Step 3: they may only ask, through
                 armPopup, requestScene, postHuClock and the key popup calls),
                 must not reach the panel (App::Panel::lib), must not touch the
                 emitter (the hu task's).
  WebUi*.cpp     httpd posts, stages and reads: it never writes the UART, never
                 runs the sink chain, never drives the emitter or the ladder,
                 and reads the matrix map only through its staged copy. Since
                 Step 3 it never calls twai_* or reinstalls the driver, and never
                 calls the panel library except through PanelOps.

foreign        (R1, docs/design/stabilisation-1-12.md) code on a stack that is
contexts       not ours: a lambda given to .onEvent(, a tud_*_cb, an IRAM_ATTR
               body. It never logs except through LOGQ; `// ctx: task <name>`
               on its first line or the line above says it runs on our task.
               NimBLE callback classes: listed as warnings, never an error.

Config.cpp     reads only the NVS keys it writes: an old key is rewritten by a
               step in src/cfg/Migrations.cpp (the recipe is in Migrations.h).

Comments are stripped before matching, so a comment may name what it forbids.
Standard library only.
"""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))


def read(rel):
    with open(os.path.join(ROOT, rel), encoding='utf-8') as f:
        return f.read()


def strip_comments(src):
    """Remove // and /* */ comments, keep strings and line numbers."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            q = c
            j = i + 1
            while j < n and src[j] != q:
                j += 2 if src[j] == '\\' else 1
            out.append(src[i:j + 1])
            i = j + 1
        elif src.startswith('//', i):
            j = src.find('\n', i)
            i = n if j < 0 else j
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append('\n' * src.count('\n', i, j))
            i = j
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def line_of(src, pos):
    return src.count('\n', 0, pos) + 1


def function_body(src, name):
    """The body of a definition `<type> name(...) { ... }`, static or in an
    anonymous namespace, with its start offset."""
    m = re.search(r'^[ \t]*(?:static[ \t]+)?(?:inline[ \t]+)?[\w:<>]+[ \t*&]+'
                  + re.escape(name) + r'\s*\([^;{]*\)\s*\{', src, re.M)
    if not m:
        return None, 0
    i = m.end() - 1
    depth, j = 0, i
    while j < len(src):
        if src[j] == '{':
            depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                return src[i:j + 1], i
        j += 1
    return None, 0


problems = []
warnings = []


def check(rel, text, pattern, why, base=0, whole=None, out=None):
    for m in re.finditer(pattern, text):
        ln = line_of(whole if whole is not None else text, base + m.start())
        (problems if out is None else out).append('%s:%d: %s  (%s)' % (rel, ln, m.group(0).strip(), why))


# ---- multi-line scans: blocks, argument lists, lambdas ----------------------------
# On comment-stripped text. String and character literals are blanked first
# (same length, so offsets and line numbers hold), so a brace or a paren in a
# format string never moves a block's end.
def mask_strings(src):
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == '\\' else 1
            out.append(c + re.sub(r'[^\n]', ' ', src[i + 1:j]) + (c if j < n else ''))
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def matching(masked, i):
    """The offset of the bracket that closes the one at i ('{' or '('), or -1."""
    o = masked[i]
    c = {'{': '}', '(': ')', '[': ']'}[o]
    depth = 0
    for j in range(i, len(masked)):
        if masked[j] == o:
            depth += 1
        elif masked[j] == c:
            depth -= 1
            if depth == 0:
                return j
    return -1


def blocks_after(masked, pattern):
    """(start, end) of every `{...}` whose opening brace ends a match of
    pattern (the pattern must end in a literal \\{)."""
    for m in re.finditer(pattern, masked):
        i = m.end() - 1
        j = matching(masked, i)
        if j > 0:
            yield m.start(), i, j + 1


def lambdas_in_call(masked, call_pattern):
    """(start, body_start, body_end) of every lambda in the argument list of a
    call matching call_pattern (which must end in a literal \\()."""
    for m in re.finditer(call_pattern, masked):
        p = m.end() - 1
        q = matching(masked, p)
        if q < 0:
            continue
        args = masked[p:q]
        for lm in re.finditer(r'\[[^\[\]]*\]\s*(?:\([^()]*\))?\s*(?:mutable\s*)?(?:->\s*[\w:<>*&\s]+?)?\{', args):
            i = p + lm.end() - 1
            j = matching(masked, i)
            if j > 0:
                yield m.start(), i, j + 1


# `// ctx: task <name>` on a construct's first line or the line above it: the
# author says it runs on a task of ours (a callback called synchronously from
# that task), so the foreign-context rule does not apply to it.
CTX_ESCAPE = re.compile(r'//\s*ctx:\s*task\s+\w+')


def escaped(raw_lines, ln):
    return any(CTX_ESCAPE.search(raw_lines[k]) for k in (ln - 2, ln - 1) if 0 <= k < len(raw_lines))


# ---- task files ---------------------------------------------------------------
TASK_FILES = [
    'src/app/KeysTask.cpp',
    'src/app/HuTask.cpp',
    'src/app/KeyBus.cpp',
    'src/sinks/BleKeyProxy.cpp',
    'src/app/PanelOwner.cpp',        # Step 3: the panel task
    'src/app/LoopTask.cpp',          # ...and its loop (main-split.md)
    'src/app/UiPolicy.cpp',
    'src/diag/CanBench.cpp',
]
for rel in TASK_FILES:
    if not os.path.exists(os.path.join(ROOT, rel)):
        continue
    src = strip_comments(read(rel))
    check(rel, src, r'\bportMAX_DELAY\b', 'a hot-path task never waits for ever')

hu = strip_comments(read('src/app/HuTask.cpp'))
check('src/app/HuTask.cpp', hu, r'\breadBytes\s*\(', 'readBytes waits on the HAL UART mutex')
check('src/app/HuTask.cpp', hu, r'\bread\s*\(\s*\)', 'a bare read() on an empty port waits 20 ms')

# ---- the glue: what runs on the keys, hu and car tasks ---------------------------
# main.cpp and the files its glue moved to (docs/design/main-split.md). Each
# function below is looked up in all of them, so a move needs no edit here.
main_rel = 'src/main.cpp'
main = strip_comments(read(main_rel))
GLUE_FILES = [main_rel, 'src/app/HuLink.cpp', 'src/app/Media.cpp', 'src/app/KeyPath.cpp',
              'src/app/PanelKeys.cpp', 'src/app/RuleActions.cpp', 'src/app/LoopTask.cpp']
GLUE = [(rel, strip_comments(read(rel))) for rel in GLUE_FILES
        if os.path.exists(os.path.join(ROOT, rel))]


def glue_function(name):
    """(file, its stripped source, the body, its offset) of a glue function."""
    for rel, src in GLUE:
        body, at = function_body(src, name)
        if body is not None:
            return rel, src, body, at
    problems.append('function %s not found in the glue files (rename? update tools/check_rules.py)' % name)
    return None, None, None, 0


KEYS_TASK_FUNCS = [
    'onAffaKeyReport', 'formatKeyLine', 'onKeyEvent', 'macroEmit', 'macroFilter', 'macroFire',
    'keysApplyRouting', 'keysApply', 'keysDeadlines', 'keysNextDue', 'writeKeyFrame',
]
HU_TASK_FUNCS = [
    'onHuFrame', 'onHuClock', 'onHuJunk', 'huWake', 'huDrainSerial', 'uartTap', 'huRecord',
]
# Render state: App::Ui's (src/app/UiPolicy.cpp) since Step 3, the ui task's
# from Step 4. The state is file-static there, so the compiler keeps other
# files out of it; what is left to check is which of its calls they make.
UI_ASKS = ('armPopup', 'releaseGlass', 'requestScene', 'postHuClock', 'queueNotification',
           'mediaChanged', 'keyPopupWanted', 'keyPopupMs', 'keyPopupShown')
RENDER_WRITES = [
    (r'\bApp::Ui::(?!(?:%s)\b)\w+' % '|'.join(UI_ASKS),
     'the render state is App::Ui\'s: only its asking calls'),
    (r'\bApp::Panel::(lib|pump|wait)\b',
     'the library object is the panel task\'s: render through PanelOps'),
]
for fn in KEYS_TASK_FUNCS + HU_TASK_FUNCS:
    rel, src, body, at = glue_function(fn)
    if body is None:
        continue
    for pat, why in RENDER_WRITES:
        check(rel, body, pat, '%s: %s' % (fn, why), at, src)
    check(rel, body, r'\b(?:[gs]_emitter|emitter\(\))\s*(?:->|\.)\s*\w+',
          '%s: the emitter is the hu task\'s (App::Hu)' % fn, at, src)
    if fn in KEYS_TASK_FUNCS:
        check(rel, body, r'\b(?:[gs]_tx|tx\(\))\s*\.\s*write(Ack|Lane)\b',
              '%s: only the hu task writes acks and lane frames' % fn, at, src)

# What runs on the car task (CarTask's callbacks). The settings are httpd's
# (Cfg::Config: one lock for the writers). A function on the car task takes a
# copy (trySnapshot) or tryLock(), never the blocking lock: a route holds it
# for as long as a settings-file import takes.
CAR_TASK_FUNCS = ['carTap', 'perform', 'onSignalsRenamed']   # perform: RuleActions
for fn in CAR_TASK_FUNCS:
    rel, src, body, at = glue_function(fn)
    if body is None:
        continue
    check(rel, body, r'\b(?:g_cfg\s*\.|s_cfg\s*->)\s*(?!tryLock\b|unlock\b|trySnapshot\b|resolveCarSigs\b)\w+',
          '%s: the settings are httpd\'s: a copy (trySnapshot) or tryLock on the car task' % fn, at, src)
    check(rel, body, r'Config::Guard', '%s: never the blocking settings lock on the car task' % fn, at, src)

# ---- the panel library: one owner (Step 3, §3.6) --------------------------------
for m in re.finditer(r'\bg_(display|carminat)\b', main):
    problems.append('%s:%d: g_%s  (the panel library is App::Panel\'s, not a global)'
                    % (main_rel, line_of(main, m.start()), m.group(1)))
PANEL_OWNERS = {'src/app/PanelOwner.cpp', 'src/app/StockRadio.cpp', 'src/app/LoopTask.cpp', main_rel}
for dirpath, _, files in os.walk(os.path.join(ROOT, 'src')):
    for fn in files:
        if not fn.endswith(('.cpp', '.h')):
            continue
        rel = os.path.relpath(os.path.join(dirpath, fn), ROOT).replace(os.sep, '/')
        if rel in PANEL_OWNERS or rel == 'src/app/PanelOwner.h':
            continue
        src = strip_comments(read(rel))
        check(rel, src, r'\bPanel::lib\s*\(',
              'only the panel task\'s files reach the library object: render through PanelOps')

# ---- httpd ------------------------------------------------------------------------
# Every route file: WebUi.cpp is the server and the core routes, the rest are
# its topics (settings, display, media, CAN, status, map, rules, ...).
for rel in sorted('src/net/' + f for f in os.listdir(os.path.join(ROOT, 'src', 'net'))
                  if f.startswith('WebUi') and f.endswith('.cpp')):
    src = strip_comments(read(rel))
    check(rel, src, r'\b_tx\s*->\s*write\w*', 'httpd never writes the UART: App::Hu::post')
    check(rel, src, r'\b_engine\s*->\s*dispatch\w*', 'httpd never runs the sink chain: App::KeyBus')
    check(rel, src, r'\b_keySink\s*->\s*sendKey', 'httpd never sends a key: App::KeyBus::postLiteral')
    check(rel, src, r'\b_emitter\s*->\s*(set\w+|send\w+|tick)', 'the emitter is the hu task\'s: stage it')
    check(rel, src, r'\b_macros\s*->\s*(run|reset|loadDefaults|fromBlob|setTable)\b',
          'the macro engine is the keys task\'s: stage or post')
    # The ladder: the keys task drives its pins and reads its map per key.
    check(rel, src, r'\b_res\s*->\s*(learnAction|learnAssert|learnCancel|allOff|setEnabled|setClasses|tick|onKey)\s*\(',
          'the resistive sink is the keys task\'s: resAllOff / resLearn')
    check(rel, src, r'\b_resMap\b', 'the ladder map is the keys task\'s: stage it (resMap)')
    check(rel, src, r'\b_map\s*->\s*\w+', 'the matrix map is the scan task\'s: _st.keymap->peek()')
    # The CAN controller and the panel are the loop's (Step 3).
    check(rel, src, r'\btwai_\w+\s*\(', 'httpd never calls the TWAI driver: Diag::CanBench')
    check(rel, src, r'\b_disp\b', 'httpd reads the panel\'s snapshot (_panel), never the library')

# ---- the log: one leveled API (docs/design/logging.md) ---------------------------
# Every line goes through LOGE..LOGT: a level and a tag, gated before it is
# formatted. LogBus is the bus under it. LogBus::line stays only where a line is
# built in pieces, until the status providers (step 3) and the head-unit UART in
# the capture (step 4) replace those builders.
# The glue builds its lines in pieces (the [hu] echo, [panelkey], [key]); the
# IDF tap (LogSetup) hands on a line the IDF formatted.
RAW_LINE_ALLOWED = set(GLUE_FILES) | {'src/app/LogSetup.cpp'}
for dirpath, _, files in os.walk(os.path.join(ROOT, 'src')):
    for fn in files:
        if not fn.endswith(('.cpp', '.h')):
            continue
        rel = os.path.relpath(os.path.join(dirpath, fn), ROOT).replace(os.sep, '/')
        if rel.startswith('src/utils/Log'):
            continue
        src = strip_comments(read(rel))
        check(rel, src, r'\bLogBus::printf\s*\(', 'log through LOGE..LOGT (a level and a tag), not LogBus::printf')
        if rel not in RAW_LINE_ALLOWED:
            check(rel, src, r'\bLogBus::line\s*\(', 'log through LOGE..LOGT (a level and a tag), not LogBus::line')

# ---- R1: foreign contexts only count, flag, post or notify (stabilisation 1.12) --
# Code that runs on a stack that is not ours: lambdas handed to an
# `.onEvent(` (WiFi's and USB's event tasks), TinyUSB's tud_*_cb, and IRAM_ATTR
# bodies (ISRs, driver callbacks). Their log is LOGQ (utils/LogQ.h): numbers
# posted, formatted later on the logbus task. A LOGI there formats a 320 B line
# on a 2 kB stack (car boots 289/290). NimBLE's callback classes run on its
# host task: their logs are reported as warnings (BLE is MeganeCAN's code, this
# release measures before it changes them). Escape: `// ctx: task <name>`.
FOREIGN_LOG = (r'\bLOG[EWIDTS]\s*\(|\bLog::(?:write|emit|printf)\s*\(|\bLogBus::(?:line|lineAt|printf)\s*\('
               r'|\bSerial\d?\s*\.\s*(?:print|printf|println|write)\b|(?<![\w.:])printf\s*\(|\bESP_LOG[EWIDV]\s*\(')
FOREIGN_SCANS = [
    ('a lambda given to onEvent: the core\'s event task', 'lambda', r'\.\s*onEvent\s*\('),
    ('a TinyUSB callback: the USB task', 'block', r'\btud_\w+_cb\s*\([^;{)]*\)\s*\{'),
    ('an IRAM_ATTR body: an ISR or a driver callback', 'block', r'\bIRAM_ATTR\b[^;{}]*?\([^;{)]*\)\s*\{'),
]
for dirpath, _, files in os.walk(os.path.join(ROOT, 'src')):
    for fn in files:
        if not fn.endswith(('.cpp', '.h')):
            continue
        rel = os.path.relpath(os.path.join(dirpath, fn), ROOT).replace(os.sep, '/')
        raw = read(rel)
        raw_lines = raw.split('\n')
        src = mask_strings(strip_comments(raw))
        for what, kind, pat in FOREIGN_SCANS:
            found = lambdas_in_call(src, pat) if kind == 'lambda' else blocks_after(src, pat)
            for start, i, j in found:
                if escaped(raw_lines, line_of(src, start)):
                    continue
                check(rel, src[i:j], FOREIGN_LOG, '%s: LOGQ, or count and log on a task of ours (R1)' % what,
                      i, src)
        # NimBLE: a report, not a rule (yet).
        for m in re.finditer(r'\b(?:class|struct)\s+(\w+)\s*(?:final\s*)?:\s*public\s+NimBLE\w*Callbacks\b[^{;]*\{', src):
            i = m.end() - 1
            j = matching(src, i)
            if j < 0 or escaped(raw_lines, line_of(src, m.start())):
                continue
            check(rel, src[i:j + 1], FOREIGN_LOG, '%s: a NimBLE callback logs on the host task' % m.group(1),
                  i, src, warnings)

# ---- CAN A: listen-only, but for the climate read ---------------------------------
# The car bus is written in one place: Mcp2518Source puts the chip in normal mode
# and sends, and only CarDiag (through Diag::Exchange's allowlist) asks it to.
TX_OWNER = 'src/bus/Mcp2518Source.cpp'
TX_CALLERS = {'src/app/CarDiag.cpp'}
for dirpath, _, files in os.walk(os.path.join(ROOT, 'src')):
    for fn in files:
        if not fn.endswith(('.cpp', '.h')):
            continue
        rel = os.path.relpath(os.path.join(dirpath, fn), ROOT).replace(os.sep, '/')
        src = strip_comments(read(rel))
        if rel != TX_OWNER:
            check(rel, src, r'\bNormal20B\b|\bg_can\s*\.\s*tryToSend\b',
                  'CAN A is listen-only: normal mode and sending live in Mcp2518Source only')
        if rel not in TX_CALLERS:
            check(rel, src, r'\bMcp2518Source\b[^;]*\.\s*(send|setTransmit)\s*\(',
                  'only CarDiag transmits on CAN A (the climate read, src/diag/ClimDiag.h)')

# ---- settings: one format, migrations in one place (src/cfg/Migrations.h) --------
# Config.cpp reads only the NVS keys it writes. An old key is rewritten into the
# new one by a step in Migrations.cpp before Config reads anything, never read
# "if the new one is missing" in Config.
cfg_rel = 'src/cfg/Config.cpp'
cfg_src = strip_comments(read(cfg_rel))
cfg_puts = set(re.findall(r'_p\.put\w+\s*\(\s*"([^"]+)"', cfg_src))
for m in re.finditer(r'_p\.(?:get\w+|isKey|remove)\s*\(\s*"([^"]+)"', cfg_src):
    if m.group(1) not in cfg_puts:
        problems.append('%s:%d: NVS key "%s" read or removed but never written  '
                        '(an old key: a step in src/cfg/Migrations.cpp, not Config)'
                        % (cfg_rel, line_of(cfg_src, m.start()), m.group(1)))

if warnings:
    print('check_rules: %d warning(s), not errors' % len(warnings))
    for w in warnings:
        print('  ' + w)
if problems:
    print('check_rules: %d problem(s)' % len(problems))
    for p in problems:
        print('  ' + p)
    sys.exit(1)
print('check_rules: ok')
