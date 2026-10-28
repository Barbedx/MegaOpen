#!/usr/bin/env python3
"""Read a board's pushed log by tag and level (docs/design/logging.md).

A line is "<ms> <L> [tag] message" (L = E W I D T; a line from before the
levelled log has no L and counts as I). Sources: files on disk, or a boot on
GitHub fetched with `gh` (the log repository, <device>/boot-NNNNN/*.log, the
parts and the flash-* files in order).

    python tools/logscan.py FILE...                         every line
    python tools/logscan.py --gh mo-4580 --boot 183         that boot from GitHub
    python tools/logscan.py --gh mo-4580 --last             the newest boot
    ... --tag hu,key --level D       only those tags, at most that level
    ... --grep CONFIRMED             a substring
    ... --tags                       how many lines per tag and level
"""
import argparse
import collections
import json
import re
import subprocess
import sys

LINE = re.compile(r'^(\d+\.\d+) (?:([EWIDT]) )?\[([^\]]+)\] ?(.*)$')
ORDER = "EWIDT"


def gh(args, raw=False):
    r = subprocess.run(["gh", "api"] + args, capture_output=True)
    if r.returncode:
        sys.exit("gh api %s: %s" % (" ".join(args), r.stderr.decode(errors="replace").strip()))
    return r.stdout.decode("utf-8", "replace") if raw else json.loads(r.stdout)


def gh_boot(repo, dev, boot):
    base = "repos/%s/contents/%s" % (repo, dev)
    if boot is None:
        boots = sorted(e["name"] for e in gh([base]) if e["name"].startswith("boot-"))
        if not boots:
            sys.exit("no boots for %s" % dev)
        name = boots[-1]
    else:
        name = "boot-%05d" % boot
    files = sorted(e["name"] for e in gh(["%s/%s" % (base, name)]) if e["name"].endswith(".log"))
    # The parts in order, then what came from flash after a reboot.
    files.sort(key=lambda n: (n.startswith("flash-"), n))
    for f in files:
        yield name + "/" + f, gh(["-H", "Accept: application/vnd.github.raw", "%s/%s/%s" % (base, name, f)], raw=True)


def lines(sources):
    for name, text in sources:
        for ln in text.splitlines():
            m = LINE.match(ln)
            if not m:
                continue
            ms, lvl, tag, msg = m.groups()
            yield name, float(ms), lvl or "I", tag, msg, ln


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*")
    ap.add_argument("--gh", metavar="DEVICE", help="read from GitHub, e.g. mo-4580")
    ap.add_argument("--repo", default="Barbedx/megaopen-logs")
    ap.add_argument("--boot", type=int)
    ap.add_argument("--last", action="store_true", help="the newest boot (with --gh)")
    ap.add_argument("--tag", help="comma-separated tags")
    ap.add_argument("--level", default="T", help="at most this level: E W I D T")
    ap.add_argument("--grep")
    ap.add_argument("--tags", action="store_true", help="count lines per tag and level")
    a = ap.parse_args()

    if a.gh:
        src = gh_boot(a.repo, a.gh, None if a.last or a.boot is None else a.boot)
    elif a.files:
        src = ((f, open(f, encoding="utf-8", errors="replace").read()) for f in a.files)
    else:
        ap.error("files or --gh")
    want = set(a.tag.split(",")) if a.tag else None
    top = ORDER.index(a.level.upper()[0])
    count = collections.Counter()
    for name, ms, lvl, tag, msg, raw in lines(src):
        if ORDER.index(lvl) > top or (want and tag not in want) or (a.grep and a.grep not in raw):
            continue
        if a.tags:
            count[(tag, lvl)] += 1
        else:
            print(raw)
    if a.tags:
        for (tag, lvl), n in sorted(count.items(), key=lambda kv: -kv[1]):
            print("%6d  %s [%s]" % (n, lvl, tag))
    return 0


if __name__ == "__main__":
    sys.exit(main())
