#include "Version.h"

// tools/version.py compiles this file, and only this file, with the define.
#ifndef MEGAOPEN_BUILD
#define MEGAOPEN_BUILD "unknown"
#endif

namespace MegaOpen
{
    const char* buildId() { return MEGAOPEN_BUILD; }
}
