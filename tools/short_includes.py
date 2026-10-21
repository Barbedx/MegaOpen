# PlatformIO pre-script: the ESP32 SDK's ~200 include directories as
# -iprefix <sdk>/ -iwithprefixbefore<rel> instead of -I<sdk>/<rel>.
#
# Why: every compile names each of those directories with its full path, and
# the framework package can sit in a long directory
# (framework-arduinoespressif32@3.20017.241212+sha..., when another version
# owns the plain name). The gcc driver reads the response file, but hands the
# whole line to cc1plus, and Windows refuses a command line over 32,767
# characters: "xtensa-...-g++: error: CreateProcess: No such file or
# directory". t2can_usbhid crossed it on 2026-10-21; t2can was ~700 short.
#
# -iwithprefixbefore puts a directory on the same search chain as -I, in the
# same command-line order, so the headers found and the binary are unchanged
# (checked: identical firmware sizes before and after). Only the -I flags
# whose directory is under the SDK root change; everything else is as SCons
# wrote it. ~19 kB shorter a compile.
Import("env")

import os
import SCons.PathList


def _sdk_root(env):
    fw = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    mcu = env.BoardConfig().get("build.mcu", "")
    if not fw or not mcu:
        return None
    return os.path.join(fw, "tools", "sdk", mcu).replace("\\", "/") + "/"


def _short_inc(paths, env, rdirs, target=None, source=None):
    root = _sdk_root(env)
    out = []
    if not paths:
        return out
    prefixed = False
    # As SCons's own _concat: substitute the path list, then RDirs.
    lst = SCons.PathList.PathList(paths).subst_path(env, target, source)
    for d in rdirs(lst):
        p = str(d)
        norm = p.replace("\\", "/")
        # The root itself (an empty tail) stays a plain -I: a bare
        # -iwithprefixbefore would take the next argument as its directory.
        if root and norm.startswith(root) and len(norm) > len(root):
            if not prefixed:
                out += ["-iprefix", root]
                prefixed = True
            out.append("-iwithprefixbefore" + norm[len(root):])
        else:
            out.append(env.subst("$INCPREFIX") + p)
    return out


env["_megaopen_short_inc"] = _short_inc
# What SCons's default _CPPINCFLAGS does, through the function above; $( $)
# keeps the flags out of the build signature, as the default does.
env["_CPPINCFLAGS"] = "$( ${_megaopen_short_inc(CPPPATH, __env__, RDirs, TARGET, SOURCE)} $)"
