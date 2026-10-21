#include "Version.h"

// tools/version.py compiles this file, and only this file, with the define.
#ifndef MEGAOPEN_BUILD
#define MEGAOPEN_BUILD "unknown"
#endif
#ifndef MEGAOPEN_ENV
#define MEGAOPEN_ENV "unknown"
#endif

namespace MegaOpen
{
    const char* buildId() { return MEGAOPEN_BUILD; }
    const char* envName() { return MEGAOPEN_ENV; }
}
