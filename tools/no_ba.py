"""Turn OFF the AFFA `BA` announce, on every build, for good.

WHY THIS IS A BUILD SCRIPT AND NOT A HAND EDIT.

The owner's instruction is that this firmware must not speak first: no `BA` sync
request, ever, under any condition. The two switches that produce one live in
AffaDisplay's `kSync` profile, and the library exposes no way to override it —
`CarminatDisplay`'s constructor takes a `CarminatHelloProfile` enum, and both
values it selects between have the announce on.

So the only place to change it is the library source. That source is a git
checkout under `.pio/libdeps/<env>/AffaDisplay`, pinned to tag v1.0.0 — and a
hand edit there is silently lost to `pio pkg update`, to a clean build, to a new
env directory, or to anyone running `git checkout` in it. It was lost exactly
that way once already, which is what this file exists to stop.

Run as a PlatformIO `extra_scripts` pre-action, it re-applies before every
compile and is idempotent. It also prints what it did, because a patch that
works silently is a patch nobody notices has stopped working.

WHAT IT CHANGES, in both Carminat sync profiles:

    announceWhenSilentMs : 30000 -> 0      no BA into a silent bus
    helloRequiresAnnounce: true  -> false  no BA before hello

The library's own comment says the panel never opens its 1C1 channel without the
announce ("4/4 in the captures, proven on hardware 2026-08-04"). That claim is
the library's, it is not ours, and the owner has asked for it to be tested rather
than assumed. If it turns out to be right, this file is one line away from being
deleted — which is the point of it being one file.
"""
Import("env")

import os
import re

MARKERS = [
    # (what to find, what to replace it with, a name for the log)
    (re.compile(r"^(\s*)30000,(\s*//.*)$", re.M),
     r"\g<1>0,     // BA ANNOUNCE DISABLED by tools/no_ba.py (was 30000)\g<2>",
     "announceWhenSilentMs -> 0"),
    (re.compile(r"^(\s*)true,(\s*//\s*(?:BA FIRST|same panel, same BA-before-hello).*)$", re.M),
     r"\g<1>false, // BA-BEFORE-HELLO DISABLED by tools/no_ba.py (was true)\g<2>",
     "helloRequiresAnnounce -> false"),
]


def patch(_source, _target, env):
    libdeps = env.subst("$PROJECT_LIBDEPS_DIR")
    envname = env.subst("$PIOENV")
    path = os.path.join(libdeps, envname, "AffaDisplay", "src", "carminat",
                        "CarminatConstants.h")
    if not os.path.isfile(path):
        # Not an error: an env without the library (env:direct) has nothing to do.
        return

    with open(path, "r", encoding="utf-8") as f:
        src = f.read()

    if "tools/no_ba.py" in src:
        print("[no_ba] BA announce already disabled in %s" % envname)
        return

    out, applied = src, []
    for rx, rep, name in MARKERS:
        out, n = rx.subn(rep, out)
        if n:
            applied.append("%s (x%d)" % (name, n))

    if not applied:
        # LOUD, not silent. If the library's wording changes, the patch stops
        # matching — and a BA going out because a regex drifted is exactly the
        # failure this script was written to prevent.
        print("[no_ba] *** WARNING: nothing matched in CarminatConstants.h — "
              "BA MAY STILL BE SENT. Check the profile by hand. ***")
        return

    with open(path, "w", encoding="utf-8") as f:
        f.write(out)
    print("[no_ba] BA announce disabled in %s: %s" % (envname, ", ".join(applied)))


env.AddPreAction("buildprog", patch)
patch(None, None, env)
