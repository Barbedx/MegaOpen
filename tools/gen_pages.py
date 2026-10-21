"""Pre-build: gzip the web pages into src/net/gen/PageGz.{h,cpp}.

The app (docs/design/web-ui.md), served at "/", is web/ — a shell web/index.html,
web/app.css and web/js/*.js — inlined into one page, PAGE_APP. web/sim/ is never
read here.

It is gzipped (level 9) into a byte array, PAGE_APP_GZ, and its length, in ONE
translation unit — one copy in flash.

The pages are the same for every env: the app reads what the build has from
/api/catalog (`features`). What this env has is written down too, for the
simulator only: build/features/<env>.json, the FEATURE_* values this build
compiles with (src/Features.h defaults, this env's -D flags, and Features.h's
"A needs B" fallbacks). tools/build_sim.py turns them into the simulator's
?env= switch through build/features.json (test_catalog: id -> macro).

Runs as a PlatformIO `extra_scripts = pre:` hook, and also standalone
(`python tools/gen_pages.py`) for a look at the numbers.

THE OUTPUT IS NOT IN GIT (.gitignore: src/net/gen/). The same pages gzip to
different bytes under different Pythons: PlatformIO's own (3.11, zlib 1.2.13)
and a newer one built with zlib-ng (3.14 on Windows) make different deflate
streams from the same input, so a tracked PageGz.cpp flipped with whichever
Python wrote it last, and every `pio run` after a hand run dirtied the tree
(and the build stamp). Every build writes it before compiling; the checks
that read it (check_pages, check_nav) say "build first" when it is missing.
"""
import gzip, json, os, re

try:                                  # under PlatformIO's SCons, __file__ is not a given
    Import("env")                     # noqa: F821
    ROOT = env["PROJECT_DIR"]         # noqa: F821
except Exception:
    ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NET  = os.path.join(ROOT, "src", "net")
GEN  = os.path.join(NET, "gen")
WEB  = os.path.join(ROOT, "web")

def env_features():
    """(env name, {FEATURE_X: 0|1}) as this build compiles, or None standalone.

    BUILD_FLAGS, not CPPDEFINES: a `pre:` script runs before PlatformIO has
    turned the flags into defines. Entries look like '-D FEATURE_X=1'."""
    try:
        name, flags = env["PIOENV"], env["BUILD_FLAGS"]          # noqa: F821
    except Exception:
        return None
    with open(os.path.join(ROOT, "src", "Features.h"), encoding="utf-8") as fp:
        fh = fp.read()
    f = {m.group(1): int(m.group(2)) for m in
         re.finditer(r'^#ifndef\s+(FEATURE_\w+)\s*\n#define\s+\1\s+(\d+)', fh, re.M)}
    for flag in flags:
        m = re.match(r'-D\s*(FEATURE_\w+)=(\d+)$', str(flag).strip())
        if m:
            f[m.group(1)] = int(m.group(2))
    # "#if A && !B / #undef A / #define A 0": FEATURE_ANCS without FEATURE_BLE.
    for m in re.finditer(r'^#if\s+(FEATURE_\w+)\s*&&\s*!(FEATURE_\w+)\s*\n#undef\s+\1\s*\n#define\s+\1\s+0',
                         fh, re.M):
        if f.get(m.group(1)) and not f.get(m.group(2)):
            f[m.group(1)] = 0
    return name, f

def write_env_features():
    got = env_features()
    if not got:
        return
    name, f = got
    out = os.path.join(ROOT, "build", "features", name + ".json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    text = json.dumps(f, sort_keys=True) + "\n"
    old = open(out, encoding="utf-8").read() if os.path.exists(out) else None
    if old != text:
        with open(out, "w", encoding="utf-8", newline="\n") as fp:
            fp.write(text)

def app_parts():
    """web/ as text: (shell, css, js). Shared with tools/build_sim.py, so the
    simulator artifact is built from exactly the bytes the board serves.

    The JS files are concatenated in FILE-NAME order into one script: plain
    browser JS, no modules and no bundler, so 00-util.js has to come first and
    99-main.js last, and the name is the only thing that says so."""
    def rd(*p):
        with open(os.path.join(WEB, *p), encoding="utf-8") as f:
            return f.read()
    jsdir = os.path.join(WEB, "js")
    js = "\n".join(rd("js", n) for n in sorted(os.listdir(jsdir)) if n.endswith(".js"))
    return rd("index.html"), rd("app.css"), js

def app_inline(shell, css, scripts):
    """Fill the shell's two placeholders. `scripts` is a list: the board gets
    one, the simulator wraps the app in its own before and after it.

    Hard failures again: a shell without a placeholder would ship a page with
    no code, and a literal `</script` inside the JS would end the tag early in
    the middle of a statement — both invisible until a browser opens it."""
    for token in ("<!--CSS-->", "<!--JS-->"):
        if shell.count(token) != 1:
            raise SystemExit("gen_pages: web/index.html needs exactly one %s" % token)
    for s in scripts:
        if "</script" in s.lower():
            raise SystemExit("gen_pages: a web/ script contains </script — split the string")
    if "</style" in css.lower():
        raise SystemExit("gen_pages: web/app.css contains </style — it would end the tag early")
    return (shell.replace("<!--CSS-->", "<style>\n" + css + "</style>")
                 .replace("<!--JS-->", "".join("<script>\n%s</script>\n" % s for s in scripts)))

def strip_comments(text):
    """Whole-line comments out of the BOARD page only: the flash pays for every
    byte of prose, and the sim keeps it for the people who read the source.

    Deliberately dumb, so it can never cut code: a line goes only when its first
    non-space characters are `//`, and a `/* ... */` block only when it STARTS a
    line (it may run over several). A comment after code on the same line stays.
    The one way to be wrong is a line inside a multi-line string or template
    literal that itself starts with `//` or `/*`; node --check on the result and
    tools/check_pages.py are the guard for that."""
    out, in_block = [], False
    for line in text.split("\n"):
        t = line.lstrip()
        if in_block:
            if "*/" in line:
                in_block = False
                rest = line.split("*/", 1)[1]
                if rest.strip():
                    out.append(rest)
            continue
        if t.startswith("//"):
            continue
        if t.startswith("/*"):
            if "*/" in t:
                rest = t.split("*/", 1)[1]
                if rest.strip():
                    out.append(rest)
            else:
                in_block = True
            continue
        out.append(line)
    return "\n".join(out)

def app_page():
    shell, css, js = app_parts()
    return app_inline(shell, strip_comments(css), [strip_comments(js)])

def main():
    os.makedirs(GEN, exist_ok=True)
    h   = ["// GENERATED by tools/gen_pages.py — do not edit. Source: web/",
           "#pragma once", "#include <stdint.h>", "#include <stddef.h>", ""]
    cpp = ["// GENERATED by tools/gen_pages.py — do not edit.",
           "#include <pgmspace.h>", '#include "PageGz.h"', ""]
    total_raw = total_gz = 0
    def sources():
        yield "PAGE_APP", app_page()
    app_gz = 0
    for name, page in sources():
        raw = page.encode("utf-8")
        gz  = gzip.compress(raw, compresslevel=9, mtime=0)
        if name == "PAGE_APP":
            app_gz = len(gz)
        total_raw += len(raw); total_gz += len(gz)
        h.append("extern const uint8_t %s_GZ[] PROGMEM;" % name)
        h.append("extern const size_t  %s_GZ_LEN;" % name)
        cpp.append("const uint8_t %s_GZ[] PROGMEM = {" % name)
        for i in range(0, len(gz), 24):
            cpp.append("    " + ",".join("0x%02x" % b for b in gz[i:i + 24]) + ",")
        cpp.append("};")
        cpp.append("const size_t %s_GZ_LEN = %d;   // %d raw" % (name, len(gz), len(raw)))
        cpp.append("")
    h.append("")
    out_h, out_cpp = os.path.join(GEN, "PageGz.h"), os.path.join(GEN, "PageGz.cpp")
    for out, lines in ((out_h, h), (out_cpp, cpp)):
        text = "\n".join(lines)
        old = open(out, encoding="utf-8").read() if os.path.exists(out) else None
        if old != text:                       # leave the mtime alone when nothing changed
            with open(out, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
    write_env_features()
    print("gen_pages: PAGE_APP %d B raw -> %d B gz" % (total_raw, app_gz))

# tools/build_sim.py imports this file for app_parts()/app_inline(), and that
# import must not rewrite src/net/gen. Under PlatformIO's SCons and standalone
# the module is never named "gen_pages", so both of those still generate.
if __name__ != "gen_pages":
    main()
