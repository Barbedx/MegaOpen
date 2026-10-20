"""Check the pages as they are SERVED, not as they are written.

    python tools/check_pages.py            # after any build

Three checks, in order of how much they have been worth:

  1. Every page's JavaScript parses (needs `node`; skipped with a note if absent).
  2. No script reaches for an element id the markup does not contain.
  3. No build-time placeholder survived into a served page.

WHY IT READS PageGz.cpp AND NOT THE SOURCES. The app is web/index.html with
`<!--CSS-->` and `<!--JS-->` filled in by tools/gen_pages.py at build time, with
web/app.css and web/js/*.js inlined and their whole-line comments stripped — so
checking web/ checks files nobody ever receives. Decompressing the gz arrays out
of the generated translation unit gives the exact bytes a browser gets: the app
(PAGE_APP) and /wiggle (PAGE_WIGGLE).

This is a syntax and wiring check. It does not open a browser and it cannot tell
you a page WORKS: that takes a browser on the live board.
Its companion is tools/check_nav.py, which proves the links resolve.
"""
import gzip, os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GZ   = os.path.join(ROOT, "src", "net", "gen", "PageGz.cpp")

# Ids a page creates at runtime are not in the markup and must not be reported.
# Kept as a list rather than by switching the check off for a whole page.
DYNAMIC = {"c0", "c1", "c2", "c3", "c4", "c5", "c6", "c7", "c8"}

def pages():
    if not os.path.exists(GZ):
        sys.exit("check_pages: no %s — build first" % GZ)
    src = open(GZ, encoding="utf-8").read()
    out = []
    for m in re.finditer(r'const uint8_t (\w+)_GZ\[\] PROGMEM = \{(.*?)\};', src, re.S):
        blob = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', m.group(2)))
        out.append((m.group(1), gzip.decompress(blob).decode("utf-8")))
    if not out:
        sys.exit("check_pages: %s has no page arrays" % GZ)
    return out

def main():
    node = shutil.which("node")
    tmp  = tempfile.mkdtemp(prefix="megaopen-pages-")
    bad  = []
    try:
        for name, html in pages():
            scripts = re.findall(r'<script>(.*?)</script>', html, re.S)

            if node:
                for i, js in enumerate(scripts):
                    p = os.path.join(tmp, "%s.%d.js" % (name, i))
                    with open(p, "w", encoding="utf-8") as f:
                        f.write(js)
                    r = subprocess.run([node, "--check", p], capture_output=True, text=True)
                    if r.returncode:
                        first = (r.stderr.strip().splitlines() or [""])[0]
                        bad.append("%s script %d does not parse: %s" % (name, i, first))

            markup = set(re.findall(r'\bid="([^"]+)"', html))
            joined = "\n".join(scripts)
            want = set()
            want |= set(re.findall(r'getElementById\(\s*[\'"]([A-Za-z0-9_-]+)[\'"]\s*\)', joined))
            want |= set(re.findall(r'querySelector\(\s*[\'"]#([A-Za-z0-9_-]+)[\'"]\s*\)', joined))

            # THE HELPER NAMES ARE READ OUT OF THE PAGE, not assumed: `$` is
            # querySelector in the app and getElementById on /wiggle. A checker
            # that only knew one form once skipped whole pages in silence. Which
            # of the two a name is decides whether the argument carries a '#'.
            for helper, kind in re.findall(
                    r'(?:const|let|var)\s+(\$|[A-Za-z_]\w*)\s*=\s*\(?\s*\w+\s*\)?\s*=>\s*'
                    r'document\.(querySelector|getElementById)\s*\(', joined):
                n = re.escape(helper)
                if kind == "querySelector":
                    want |= set(re.findall(n + r'\(\s*[\'"]#([A-Za-z0-9_-]+)[\'"]\s*\)', joined))
                else:
                    want |= set(re.findall(n + r'\(\s*[\'"]([A-Za-z0-9_-]+)[\'"]\s*\)', joined))
            for missing in sorted(want - markup - DYNAMIC):
                bad.append("%s asks for #%s, which is not in its markup" % (name, missing))

            for token in ("<!--CSS-->", "<!--JS-->"):
                if token in html:
                    bad.append("%s still contains %s — gen_pages did not substitute it"
                               % (name, token))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    for b in bad:
        print("FAIL " + b)
    if bad:
        sys.exit(1)
    n = len(pages())
    print("check_pages: %d pages served clean%s"
          % (n, "" if node else " (node not found — JS was NOT parsed)"))

main()
