"""The page's words: web/i18n/<lang>.json against each other and against web/.

    python tools/check_i18n.py            the checks below; exit 1 on an error
    python tools/check_i18n.py --strict   also fails on Ukrainian left in the code

Errors (always):
  * the languages have different keys, or a value is empty;
  * a value's {0}, {1}... differ between the languages (a translation that
    drops an argument shows the wrong number, or none);
  * the code names a key no dictionary has: t('x.y'), data-i18n="x.y",
    data-i18n-aria="x.y" with a literal key;
  * the firmware sends a code the page has no words for: an error reply's
    text/plain code (e.<code>) or a /api/health chain code (w.<code>; the
    *_ok codes may have none);
  * the catalog (build/catalog.json, from `pio test -e native -f test_catalog`)
    has a name the page cannot put in words: a Ukrainian name, a raise group
    or a unit whose key (as i18nCat() in web/js/20-kit.js derives it) is not in
    uk.json. Skipped, with a note, when the file is not there.
Warnings:
  * a key nothing names literally and no dynamic prefix covers (KEY_PREFIXES);
  * Ukrainian (Cyrillic) still in web/js/*.js or web/index.html outside comments,
    counted per file: the migration to keys is page by page
    (docs/design/web-ui.md, "Languages"). --strict makes this an error.
"""
import json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "web")
LANGS = ("uk", "en")
# Keys built at run time ('w.'+code, 's.'+state, 'e.'+code, a screen's title key).
KEY_PREFIXES = ("w.", "s.", "e.", "k.", "nav.")
CATALOG = os.path.join(ROOT, "build", "catalog.json")
SRC = os.path.join(ROOT, "src")

def load(lang):
    with open(os.path.join(WEB, "i18n", lang + ".json"), encoding="utf-8") as f:
        return json.load(f)

def strip_comments(js):
    """/* ... */ anywhere and whole-line // comments: what a reader sees, not
    what the page shows. Good enough for this code base: no '/*' in strings."""
    js = re.sub(r"/\*.*?\*/", "", js, flags=re.S)
    return "\n".join(l for l in js.split("\n") if not l.lstrip().startswith("//"))

NEED = object()

def catalog_keys(c):
    """(key, what) for every name the page translates, the way i18nCat() does:
    what is the firmware's text, or NEED where a key is needed whatever it says
    (a name or label the firmware left out: the page has the words)."""
    out = []
    def rows(lst, dom, base):
        for r in lst or []:
            v = str(r.get("v"))
            if dom == "sig":
                out.append((("k.sig." + v, "k.raise." + v), r.get("name", NEED)))
            else:
                out.append((("k.%s.%s" % (dom, v)) if dom else ("%s.%s" % (base, v)), r.get("name", NEED)))
    for st in c.get("settings", []):
        out.append(("k.set." + st["id"], NEED))   # no label in the catalog: the page has the words
        rows(st.get("enum"), st.get("dom"), "k.set." + st["id"])
        for col in st.get("cols") or []:
            out.append(("k.set.%s.%s" % (st["id"], col["id"]), NEED))
            rows(col.get("enum"), col.get("dom"), "k.set.%s.%s" % (st["id"], col["id"]))
    for n, lst in (c.get("enums") or {}).items():
        rows(lst, n, None)
    for l in c.get("layouts", []):
        out.append(("k.layout." + l["id"], l.get("name", NEED)))
        for prm in l.get("params", []):
            out.append(("k.layout.%s.%s" % (l["id"], prm["n"]), prm.get("label", NEED)))
            out.append(("k.layout.%s.%s.h" % (l["id"], prm["n"]), prm.get("hint")))
    for lst, d in (("styles", "style"), ("scenes", "scene"), ("bleModes", "ble"), ("panels", "panel"),
                   ("dests", "dest"), ("actions", "action"), ("panelKeys", "pkey")):
        for x in c.get(lst, []):
            out.append(("k.%s.%s" % (d, x["id"]), x.get("name", NEED)))
    for x in c.get("classes", []):
        out.append(("k.class.%s" % x["bit"], x.get("name", NEED)))
    for x in c.get("console", []):
        out.append(("k.con." + x["cmd"], x.get("name", NEED)))
        for f in x.get("fields", []):
            out.append(("k.con.%s.%s" % (x["cmd"], f["n"]), f.get("label", NEED)))
    for x in c.get("raise", []):
        out.append(("k.raise." + x["id"], x.get("name", NEED)))
        out.append(("k.raise.%s.why" % x["id"], x.get("why")))
        out.append(("k.rgroup." + x["group"], NEED))
        if x.get("unit"):
            out.append(("k.unit." + x["unit"], NEED))
    return out

def main():
    strict = "--strict" in sys.argv
    errors, warnings = [], []
    d = {l: load(l) for l in LANGS}
    base = d["uk"]
    for l in LANGS[1:]:
        for k in sorted(set(base) - set(d[l])):
            errors.append("%s: no key %s" % (l, k))
        for k in sorted(set(d[l]) - set(base)):
            errors.append("%s: key %s is not in uk" % (l, k))
    args = re.compile(r"\{\d\}")
    for k in base:
        for l in LANGS:
            if k in d[l] and not str(d[l][k]).strip():
                errors.append("%s: %s is empty" % (l, k))
        want = sorted(args.findall(base[k]))
        for l in LANGS[1:]:
            if k in d[l] and sorted(args.findall(d[l][k])) != want:
                errors.append("%s: %s has %s, uk has %s" % (l, k, sorted(args.findall(d[l][k])), want))

    files = [os.path.join(WEB, "js", n) for n in sorted(os.listdir(os.path.join(WEB, "js"))) if n.endswith(".js")]
    files.append(os.path.join(WEB, "index.html"))
    used = set()
    lit = re.compile(r"""\bt\(\s*'([A-Za-z]+\.[A-Za-z0-9_.]+)'|data-i18n(?:-aria)?="([^"]+)"|title:'([a-z]+\.[A-Za-z0-9_]+)'""")
    cyr = re.compile(r"[Ѐ-ӿ]")
    left = []
    for path in files:
        with open(path, encoding="utf-8") as f:
            src = f.read()
        code = strip_comments(src) if path.endswith(".js") else re.sub(r"<!--.*?-->", "", src, flags=re.S)
        for m in lit.finditer(code):
            k = m.group(1) or m.group(2) or m.group(3)
            if k.endswith("."):
                continue                    # a prefix the code completes: t('k.sig.'+id)
            used.add(k)
            if k not in base:
                errors.append("%s: names key %s, which uk.json does not have" % (os.path.relpath(path, ROOT), k))
        n = sum(1 for line in code.split("\n") if cyr.search(line))
        if n:
            left.append((os.path.relpath(path, ROOT).replace(os.sep, "/"), n))
    # The firmware's side: codes it sends that the page must have words for.
    for dirpath, _, names in os.walk(SRC):
        if os.sep + "gen" in dirpath:
            continue
        for n in names:
            if not n.endswith((".cpp", ".h")):
                continue
            path = os.path.join(dirpath, n)
            with open(path, encoding="utf-8", errors="replace") as f:
                src = f.read()
            rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
            for m in re.finditer(r'"text/plain", "(?:[^"]*: )?([a-z][a-z0-9_]*)(?: [^"]*)?"\)', src):
                if "e." + m.group(1) not in base:
                    errors.append("%s: replies with code %s, which uk.json has no e.%s for" % (rel, m.group(1), m.group(1)))
            if n == "WebUi.cpp" or n == "ClockHealth.h":
                for m in re.finditer(r'"((?:panel|media|clock|keys|ancs|hu|knobs|lights|phone)_[a-z_]+)"', src):
                    c = m.group(1)
                    if not c.endswith("_ok") and "w." + c not in base and "e." + c not in base:
                        errors.append("%s: health code %s has no w.%s in uk.json" % (rel, c, c))
    cyr_s = lambda v: isinstance(v, str) and cyr.search(v)
    if os.path.exists(CATALOG):
        with open(CATALOG, encoding="utf-8") as f:
            cat = json.load(f)
        missing = set()
        for key, what in catalog_keys(cat):
            keys = key if isinstance(key, tuple) else (key,)
            if (what is NEED or cyr_s(what)) and not any(k in base for k in keys):
                missing.add(keys[0])
        for k in sorted(missing):
            errors.append("catalog: no words for %s (uk.json)" % k)
    else:
        print("check_i18n: note: no build/catalog.json, catalog not checked (pio test -e native -f test_catalog)")
    for k in base:
        if k not in used and not k.startswith(KEY_PREFIXES):
            warnings.append("unused key %s" % k)
    if left:
        msg = "Ukrainian still in the code (lines): " + ", ".join("%s %d" % x for x in left)
        (errors if strict else warnings).append(msg)

    for w in warnings:
        print("check_i18n: warning: " + w)
    for e in errors:
        print("check_i18n: ERROR: " + e)
    if errors:
        sys.exit(1)
    print("check_i18n: ok, %d keys in %s" % (len(base), ", ".join(LANGS)))

main()
