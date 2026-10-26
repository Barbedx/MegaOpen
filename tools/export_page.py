#!/usr/bin/env python3
"""The settings page as files, for the apps that carry their own copy of it.

    python tools/export_page.py OUTDIR

Writes OUTDIR/index.html (the same page the board serves at "/", PAGE_APP),
OUTDIR/i18n/en.json (PAGE_I18N_EN) and OUTDIR/page.json ({"fw": version}), the
version the page was built with, MEGAOPEN_VERSION (src/Version.h). Built by
tools/gen_pages.py's own functions, so the copies cannot drift from the board's.
MegaOpen Companion runs this at build time (android/app/build.gradle.kts).
Standard library only.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_pages  # noqa: E402  (named "gen_pages": it does not rewrite src/net/gen)


def version():
    with open(os.path.join(gen_pages.ROOT, "src", "Version.h"), encoding="utf-8") as f:
        m = re.search(r'#define MEGAOPEN_VERSION "([0-9.]+)"', f.read())
    return m.group(1) if m else "0.0.0"


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
    if old != text:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    write(os.path.join(out, "index.html"), gen_pages.app_page())
    write(os.path.join(out, "i18n", "en.json"), gen_pages.i18n_min("en"))
    write(os.path.join(out, "page.json"), json.dumps({"fw": version()}) + "\n")
    print("export_page: %s (fw %s)" % (out, version()))


if __name__ == "__main__":
    main()
