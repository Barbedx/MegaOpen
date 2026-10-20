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
  the panel      (Step 3) AffaDisplay is reached only through App::Panel::lib()
                 or carminat(), and only from the panel task's files:
                 PanelOwner.cpp, UiPolicy.cpp and main.cpp (setup and loop, never
                 a keys or hu function). No g_display / g_carminat globals.
  HuTask.cpp     - no readBytes() and no bare read(): the HAL UART mutex is
                   shared with the key writes, and both of those can wait on it
                   (§3.3). read(buf, n) after available() is the only form.
  main.cpp       functions that run on the keys or hu task must not write the
                 render state (App::Ui's since Step 3: they may only ask, through
                 armPopup, requestScene, postHuClock and the key popup calls),
                 must not reach the panel (App::Panel::lib), must not touch the
                 emitter (the hu task's).
  WebUi*.cpp     httpd posts, stages and reads: it never writes the UART, never
                 runs the sink chain, never drives the emitter or the ladder,
                 and reads the matrix map only through its staged copy. Since
                 Step 3 it never calls twai_* or reinstalls the driver (bench ops
                 are Diag::CanBench requests) and never calls the panel library.

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


def check(rel, text, pattern, why, base=0, whole=None):
    for m in re.finditer(pattern, text):
        ln = line_of(whole if whole is not None else text, base + m.start())
        problems.append('%s:%d: %s  (%s)' % (rel, ln, m.group(0).strip(), why))


# ---- task files ---------------------------------------------------------------
TASK_FILES = [
    'src/app/KeysTask.cpp',
    'src/app/HuTask.cpp',
    'src/app/KeyBus.cpp',
    'src/sinks/BleKeyProxy.cpp',
    'src/app/PanelOwner.cpp',        # Step 3: the panel task
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

# ---- main.cpp: what runs on the keys and hu tasks -------------------------------
main_rel = 'src/main.cpp'
main = strip_comments(read(main_rel))

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
    (r'\bApp::Panel::(lib|carminat|pump|resync|serviceTx)\b',
     'AffaDisplay is the panel task\'s: post to the mailbox'),
]
for fn in KEYS_TASK_FUNCS + HU_TASK_FUNCS:
    body, at = function_body(main, fn)
    if body is None:
        problems.append('%s: function %s not found (rename? update tools/check_rules.py)' % (main_rel, fn))
        continue
    for pat, why in RENDER_WRITES:
        check(main_rel, body, pat, '%s: %s' % (fn, why), at, main)
    check(main_rel, body, r'\bg_emitter\s*\.\s*\w+', '%s: the emitter is the hu task\'s (App::Hu)' % fn, at, main)
for fn in KEYS_TASK_FUNCS:
    body, at = function_body(main, fn)
    if body is None:
        continue
    check(main_rel, body, r'\bg_tx\s*\.\s*write(Ack|Lane)\b',
          '%s: only the hu task writes acks and lane frames' % fn, at, main)

# ---- the panel library: one owner (Step 3, §3.6) --------------------------------
for m in re.finditer(r'\bg_(display|carminat)\b', main):
    problems.append('%s:%d: g_%s  (the panel library is App::Panel\'s, not a global)'
                    % (main_rel, line_of(main, m.start()), m.group(1)))
PANEL_OWNERS = {'src/app/PanelOwner.cpp', 'src/app/UiPolicy.cpp', main_rel}
for dirpath, _, files in os.walk(os.path.join(ROOT, 'src')):
    for fn in files:
        if not fn.endswith(('.cpp', '.h')):
            continue
        rel = os.path.relpath(os.path.join(dirpath, fn), ROOT).replace(os.sep, '/')
        if rel in PANEL_OWNERS or rel == 'src/app/PanelOwner.h':
            continue
        src = strip_comments(read(rel))
        check(rel, src, r'\bPanel::(lib|carminat)\s*\(',
              'only the panel task\'s files may reach AffaDisplay')

# ---- httpd ------------------------------------------------------------------------
for rel in ['src/net/WebUi.cpp', 'src/net/WebUiMacro.cpp', 'src/net/WebUiRes.cpp',
            'src/net/WebUiKnobs.cpp']:
    if not os.path.exists(os.path.join(ROOT, rel)):
        continue
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
    check(rel, src, r'\bTwaiBus::instance\s*\(\s*\)\s*\.\s*(begin|end|restart|setMode|setTiming|setTxEnabled|'
                    r'poll|recvOne|waitRx|maintain|clearTx|send|trySend)\s*\(',
          'httpd never drives the TWAI driver: Diag::CanBench')
    check(rel, src, r'\b_disp\b', 'httpd reads the panel\'s snapshot (_panel), never the library')

if problems:
    print('check_rules: %d problem(s)' % len(problems))
    for p in problems:
        print('  ' + p)
    sys.exit(1)
print('check_rules: ok')
