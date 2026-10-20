"""Build the UI simulator: the board's page plus a simulated board, one HTML file.

    python tools/build_sim.py            # -> build/ui-sim.html

WHY IT EXISTS. The UI is designed and reviewed in a browser with no board on the
desk (docs/design/web-ui.md). The simulator has to be the SAME page the board
serves, not a copy that drifts: so the app half comes from tools/gen_pages.py's
app_parts(), byte for byte, and only web/sim/ is added around it.

ORDER MATTERS, and the file names cannot say it, so it is said here:
  - web/sim/mock.js goes BEFORE the app. It sets window.SIM, and 10-api.js
    checks for that on every call() — the app must find it from the first one.
  - every other web/sim/*.js goes AFTER the app, in name order. Those are
    extensions (audit.js adds the Аудит screen to SCR) and need the app's
    globals to exist when they run.
The board build never reads web/sim/, so none of this reaches the flash.

THE CATALOG is not in web/sim/: it is build/catalog.json, written by the
firmware's own Catalog::write() in env:native (test/test_catalog), and set as
window.SIM_CATALOG ahead of mock.js. So the simulator shows the lists the board
serves at /api/catalog, and a new style or action needs no edit here.
build/decode.json, the factory decode map in /api/map's `can` shape plus the
signal names (test/test_decode, the firmware's own canJson), is
window.SIM_DECODE the same way.

THE ENVS. `?env=devkit` (or t2can, direct, ...) runs the simulator as that
board build: the catalog's `features` and the routes that exist. The truth is
the board build's own: tools/gen_pages.py writes build/features/<env>.json (the
FEATURE_* values that env compiles with) during `pio run -e <env>`, and
build/features.json (test_catalog) maps each catalog feature id to its macro.
Only envs that have the web server (web_envs(): platformio.ini, `extends`
followed) are offered. One with no file, or a file older than platformio.ini,
is warned about: build it (`pio run -e <env>`) and run this again.
With none, every feature is on.
"""
import configparser, json, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import gen_pages                                   # noqa: E402  (does not regenerate: see its last lines)

SIM = os.path.join(ROOT, "web", "sim")
OUT = os.path.join(ROOT, "build", "ui-sim.html")
CATALOG = os.path.join(ROOT, "build", "catalog.json")
DECODE = os.path.join(ROOT, "build", "decode.json")
FEATURES = os.path.join(ROOT, "build", "features.json")
ENVS_DIR = os.path.join(ROOT, "build", "features")

INI = os.path.join(ROOT, "platformio.ini")

def web_envs():
    """The board envs that compile the web server: not native, and a
    build_src_filter (own or through `extends`) that is absent or takes +<*>
    or +<net/>. min, bare, boardv and ledtest have none."""
    cp = configparser.ConfigParser(interpolation=None, strict=False)
    cp.read(INI, encoding="utf-8")
    def get(sec, key, seen=()):
        if not cp.has_section(sec) or sec in seen:
            return None
        if cp.has_option(sec, key):
            return cp.get(sec, key)
        ext = cp.get(sec, "extends", fallback="").strip()
        return get(ext, key, seen + (sec,)) if ext else None
    out = []
    for sec in cp.sections():
        if not sec.startswith("env:") or (get(sec, "platform") or "").strip() == "native":
            continue
        flt = get(sec, "build_src_filter")
        if flt is None or "+<*>" in flt or "+<net/" in flt:
            out.append(sec[4:])
    return out

def sim_envs(warn=False):
    """{env: {feature id: bool}} for every web env built so far."""
    if not os.path.exists(FEATURES):
        if warn:
            print("build_sim: WARNING no build/features.json — run `pio test -e native -f test_catalog`")
        return {}
    with open(FEATURES, encoding="utf-8") as f:
        macro = json.load(f)
    ini_t = os.path.getmtime(INI)
    out = {}
    for env in web_envs():
        p = os.path.join(ENVS_DIR, env + ".json")
        if not os.path.exists(p):
            if warn:
                print("build_sim: WARNING env:%s has a web server but no build/features/%s.json "
                      "— `pio run -e %s` first" % (env, env, env))
            continue
        if warn and os.path.getmtime(p) < ini_t:
            print("build_sim: WARNING build/features/%s.json is older than platformio.ini "
                  "— rebuild env:%s" % (env, env))
        with open(p, encoding="utf-8") as f:
            flags = json.load(f)
        out[env] = {fid: bool(flags.get(m, 0)) for fid, m in macro.items()}
    return dict(sorted(out.items()))

def main():
    if not os.path.exists(CATALOG):
        sys.exit("build_sim: no build/catalog.json — run `pio test -e native -f test_catalog` first")
    if not os.path.exists(DECODE):
        sys.exit("build_sim: no build/decode.json — run `pio test -e native -f test_decode` first")
    with open(CATALOG, encoding="utf-8") as f:
        catalog = json.dumps(json.load(f), ensure_ascii=False, separators=(",", ":"))
    with open(DECODE, encoding="utf-8") as f:
        decode = json.dumps(json.load(f), ensure_ascii=False, separators=(",", ":"))
    shell, css, app = gen_pages.app_parts()
    def rd(n):
        with open(os.path.join(SIM, n), encoding="utf-8") as f:
            return f.read()
    after = [rd(n) for n in sorted(os.listdir(SIM)) if n.endswith(".js") and n != "mock.js"]
    # "</" escaped so no string in the catalog can close the inline <script>.
    envs_d = sim_envs(warn=True)
    envs = json.dumps(envs_d, separators=(",", ":"))
    sim = "window.SIM_CATALOG=%s;\nwindow.SIM_DECODE=%s;\nwindow.SIM_ENVS=%s;\n%s" % (
        catalog.replace("</", "<\\/"), decode.replace("</", "<\\/"), envs, rd("mock.js"))
    html = gen_pages.app_inline(shell, css, [sim, app] + after)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(html)
    print("build_sim: %s, %d B, envs: %s" % (os.path.relpath(OUT, ROOT), len(html.encode("utf-8")),
                                             ", ".join(envs_d) or "none (all features on)"))

main()
