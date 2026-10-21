"""Pre-build: stamp the firmware with the commit it was built from.

MEGAOPEN_VERSION in src/Version.h is the same "1.0.0" in every image, so a log
line could never say which flash it came from. This passes

    -D MEGAOPEN_BUILD="<git short hash>[-dirty]@<build time, local, minutes>"

e.g. 61e6364-dirty@2026-10-09T14:03. "-dirty" means the working tree had
uncommitted changes to tracked files. What a build writes is not tracked
(src/net/gen/, build/), so a build cannot make its own stamp dirty.

ONLY src/Version.cpp is compiled with the define, through a build middleware.
On every TU it would rebuild the whole firmware and every library whenever the
time moved, i.e. on every build; on one TU it costs one small compile and a
relink. Everything else asks MegaOpen::buildId() for the string.

Runs as a PlatformIO `extra_scripts = pre:` hook (after gen_pages.py), and
standalone (`python tools/version.py`) to print what it would stamp.
"""
import os
import subprocess
import time

try:                                  # under PlatformIO's SCons, __file__ is not a given
    Import("env")                     # noqa: F821
    ENV = env                         # noqa: F821
    ROOT = ENV["PROJECT_DIR"]
except Exception:
    ENV = None
    ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def git(*args):
    try:
        out = subprocess.run(["git"] + list(args), cwd=ROOT, capture_output=True,
                             text=True, timeout=10)
        return out.stdout.strip() if out.returncode == 0 else None
    except Exception:
        return None


def build_id():
    rev = git("rev-parse", "--short", "HEAD") or "nogit"
    # Tracked files only: an untracked note in docs/ is not part of the image.
    status = git("status", "--porcelain", "--untracked-files=no")
    dirty = "-dirty" if status else ""
    return "%s%s@%s" % (rev, dirty, time.strftime("%Y-%m-%dT%H:%M"))


if ENV is None:
    print(build_id())
elif ENV.get("PIOPLATFORM") == "native":
    pass                              # host tests have no Version.cpp and no use for it
else:
    BUILD = build_id()
    TARGET = os.path.normcase(os.path.abspath(os.path.join(ROOT, "src", "Version.cpp")))
    print("MegaOpen build id: %s" % BUILD)

    def stamp(env, node):
        try:
            path = os.path.normcase(os.path.abspath(node.srcnode().get_abspath()))
        except Exception:
            return node
        if path != TARGET:
            return node
        defines = list(env.get("CPPDEFINES", []))
        defines.append(("MEGAOPEN_BUILD", env.StringifyMacro(BUILD)))
        # The env name picks this image's asset in a GitHub release (src/net/GhOta.cpp).
        defines.append(("MEGAOPEN_ENV", env.StringifyMacro(env["PIOENV"])))
        return env.Object(node, CPPDEFINES=defines)

    ENV.AddBuildMiddleware(stamp)
