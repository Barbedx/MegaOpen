#!/usr/bin/env python3
"""Fetch a board's CAN recording from the log repository and join the parts.

The board sends each closed /cap file as <device>/cap/<session>/<file>-<part>.bin
(src/net/LogPush.h). This puts <out>/<session>/<file>.bin back together, ready
for tools/can_dump.py. Uses the GitHub CLI (`gh`), logged in with read access.

    python tools/cap_fetch.py mo-4580                 # every session
    python tools/cap_fetch.py mo-4580 --session 12    # one
    python tools/cap_fetch.py mo-4580 --repo Barbedx/megaopen-logs --out caps
"""
import argparse
import json
import os
import re
import subprocess
import sys


def gh(args, raw=False):
    r = subprocess.run(["gh", "api"] + args, capture_output=True)
    if r.returncode:
        sys.exit("gh api %s: %s" % (" ".join(args), r.stderr.decode(errors="replace").strip()))
    return r.stdout if raw else json.loads(r.stdout)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("device", help="the board's folder, e.g. mo-4580")
    ap.add_argument("--repo", default="Barbedx/megaopen-logs")
    ap.add_argument("--session", type=int, help="only this session")
    ap.add_argument("--out", default="caps")
    a = ap.parse_args()

    base = "repos/%s/contents/%s/cap" % (a.repo, a.device)
    sessions = [e["name"] for e in gh([base]) if e["type"] == "dir"]
    if a.session is not None:
        sessions = [s for s in sessions if int(s) == a.session]
    for s in sorted(sessions):
        parts = {}
        for e in gh(["%s/%s" % (base, s)]):
            m = re.fullmatch(r"(\d+)-(\d+)\.bin", e["name"])
            if m:
                parts.setdefault(m.group(1), []).append((int(m.group(2)), e["name"]))
        os.makedirs(os.path.join(a.out, s), exist_ok=True)
        for f, ps in sorted(parts.items()):
            ps.sort()
            want = list(range(len(ps)))
            if [p for p, _ in ps] != want:
                print("session %s file %s: parts %s, some missing (still being sent?)" % (s, f, [p for p, _ in ps]))
            path = os.path.join(a.out, s, f + ".bin")
            with open(path, "wb") as out:
                for _, name in ps:
                    out.write(gh(["-H", "Accept: application/vnd.github.raw", "%s/%s/%s" % (base, s, name)], raw=True))
            print("%s: %d part(s), %d bytes" % (path, len(ps), os.path.getsize(path)))


if __name__ == "__main__":
    main()
