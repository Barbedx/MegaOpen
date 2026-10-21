"""The page's words: web/i18n/<lang>.json against each other and against web/.

    python tools/check_i18n.py            the checks below; exit 1 on an error
    python tools/check_i18n.py --strict   also fails on Ukrainian left in the code

Errors (always):
  * the languages have different keys, or a value is empty;
  * a value's {0}, {1}... differ between the languages (a translation that
    drops an argument shows the wrong number, or none);
  * the code names a key no dictionary has: t('x.y'), data-i18n="x.y",
    data-i18n-aria="x.y" with a literal key.
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
# Keys built at run time ('w.'+code, 's.'+state, a screen's title key).
KEY_PREFIXES = ("w.", "s.", "nav.")

def load(lang):
    with open(os.path.join(WEB, "i18n", lang + ".json"), encoding="utf-8") as f:
        return json.load(f)

def strip_comments(js):
    """/* ... */ anywhere and whole-line // comments: what a reader sees, not
    what the page shows. Good enough for this code base: no '/*' in strings."""
    js = re.sub(r"/\*.*?\*/", "", js, flags=re.S)
    return "\n".join(l for l in js.split("\n") if not l.lstrip().startswith("//"))

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
            used.add(k)
            if k not in base:
                errors.append("%s: names key %s, which uk.json does not have" % (os.path.relpath(path, ROOT), k))
        n = sum(1 for line in code.split("\n") if cyr.search(line))
        if n:
            left.append((os.path.relpath(path, ROOT).replace(os.sep, "/"), n))
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
