"""Every page link the app has leads to a route this build actually has.

Run after a build:  python tools/check_nav.py [env]     (default: direct)

WHAT IT PROVES, per env:
  1. "/" serves the app: WebUi.cpp registers "/" with PAGE_APP_GZ, and the array
     is in the .elf.
  2. Every page the app links to (href="/..." in the served PAGE_APP; /update is
     ElegantOTA's own and skipped) is in this binary. Today there is none.
  3. The service console: every cmd in the catalog has a route.
  4. The layout parameters: every one is reported and assigned by /api/media.

HOW (2). A page route is the only thing that references its PAGE_<X>_GZ array.
With the route compiled out nothing references it and --gc-sections drops it,
so a page whose symbol is absent from the .elf is a page with no route. The
linker knows the #if blocks better than a reading of the .cpp would.

THE CONSOLE. The service console's commands are a table in src/net/Catalog.cpp
(served in /api/catalog), and the page posts each to /api/display/<cmd>. The
routes are hand-written in WebUi.cpp, so every cmd in build/catalog.json
(written by `pio test -e native -f test_catalog`) must name one there.

THE LAYOUT PARAMETERS. The page renders each layout's `params` from the
catalog and posts them to /api/media, then reads them back from GET /api/media.
Each param name must be both written by the GET handler (d["n"] = ...) and
assigned by the POST handler (m.n = ..., or the setter setN(...)) in WebUi.cpp.
String presence, no more. Without build/catalog.json neither the console nor
the params can be checked, so that is a failure, not a note.
"""
import glob, gzip, json, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENV  = sys.argv[1] if len(sys.argv) > 1 else "direct"
ELF  = os.path.join(ROOT, ".pio", "build", ENV, "firmware.elf")
WEBUI = os.path.join(ROOT, "src", "net", "WebUi.cpp")

# page path -> the gz array its route serves
SYM  = {"/": "PAGE_APP"}
SKIP = {"/update"}

def nm_symbols(elf):
    pats = ["toolchain-xtensa-esp32*/bin/xtensa-esp32-elf-nm*",
            "toolchain-*riscv*/bin/riscv32-esp-elf-nm*"]
    for pat in pats:
        for nm in glob.glob(os.path.join(os.path.expanduser("~"), ".platformio", "packages", pat)):
            try:
                out = subprocess.run([nm, elf], capture_output=True, text=True, timeout=120)
                if out.returncode == 0:
                    return {l.split()[-1] for l in out.stdout.splitlines() if l.split()}
            except Exception:
                pass
    return None

def app_links():
    gz = os.path.join(ROOT, "src", "net", "gen", "PageGz.cpp")
    src = open(gz, encoding="utf-8").read()
    m = re.search(r'const uint8_t PAGE_APP_GZ\[\] PROGMEM = \{(.*?)\};', src, re.S)
    if not m:
        sys.exit("check_nav: PAGE_APP_GZ not in PageGz.cpp — build first")
    data = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', m.group(1)))
    html = gzip.decompress(data).decode("utf-8")
    return sorted(set(re.findall(r'href="(/[^"#?]*)', html)))

def root_serves_app(web):
    m = re.search(r'route\("/",\s*HTTP_GET,.*?\}\);', web, re.S)
    if not m or "PAGE_APP_GZ" not in m.group(0):
        return ['"/" — WebUi.cpp does not serve PAGE_APP_GZ there']
    return []

def console_missing(web):
    cat = os.path.join(ROOT, "build", "catalog.json")
    if not os.path.exists(cat):
        return ["build/catalog.json missing — run `pio test -e native -f test_catalog` first"]
    cmds = [c["cmd"] for c in json.load(open(cat, encoding="utf-8"))["console"]]
    return ["console %s — no route \"/api/display/%s\" in WebUi.cpp" % (c, c)
            for c in cmds if '"/api/display/%s"' % c not in web]

def layout_params_missing(web):
    cat = os.path.join(ROOT, "build", "catalog.json")
    if not os.path.exists(cat):
        return []
    a = web.find('route("/api/media", HTTP_GET')
    b = web.find('route("/api/media", HTTP_POST')
    if a < 0 or b < 0:
        return ["layout params — the /api/media handlers are not in WebUi.cpp"]
    get, post = web[a:b], web[b:web.find("route(", b + 1)]
    names = sorted({f["n"] for l in json.load(open(cat, encoding="utf-8"))["layouts"]
                    for f in l.get("params", [])})
    bad = []
    for n in names:
        if not re.search(r'd\["%s"\]\s*=[^=]' % re.escape(n), get):
            bad.append('layout param %s — GET /api/media never reports it' % n)
        setter = "set" + n[0].upper() + n[1:] + "("
        if not re.search(r'(?<![\w.])m\.%s\s*=[^=]' % re.escape(n), post) and setter not in post:
            bad.append('layout param %s — POST /api/media never assigns it' % n)
    return bad

def main():
    if not os.path.exists(ELF):
        sys.exit("check_nav: no %s — build env:%s first" % (ELF, ENV))
    syms = nm_symbols(ELF)
    if syms is None:
        sys.exit("check_nav: could not run nm from the PlatformIO toolchain")
    web = open(WEBUI, encoding="utf-8").read()
    bad = root_serves_app(web)
    links = app_links()
    for href in ["/"] + links:
        if href in SKIP:
            continue
        s = SYM.get(href)
        if s is None:
            bad.append("%s — the app links it, but check_nav has no symbol for it" % href)
        elif (s + "_GZ") not in syms:
            bad.append("%s — %s_GZ is not in the binary (route compiled out)" % (href, s))
    bad += console_missing(web)
    bad += layout_params_missing(web)
    for b in bad:
        print("FAIL " + b)
    if bad:
        sys.exit(1)
    print("check_nav: env:%s — \"/\" is the app; its links (%s) have routes"
          % (ENV, ", ".join(links) or "none"))

main()
