#pragma once

// The single firmware version string (not the NVS config schema number).
// Semver against the head unit: the major moves when the Raise/canbox protocol
// changes in a way a head unit would have to follow.
#define MEGAOPEN_VERSION "1.2.0"

// Wire field: Rav4Emitter's MSG_VERSION is exactly 16 bytes, space-padded.
// Written out literally so it cannot overrun the field.
#define MEGAOPEN_VERSION_WIRE "MegaOpen 1.2.0  "
static_assert(sizeof(MEGAOPEN_VERSION_WIRE) == 17,
              "MSG_VERSION is a fixed 16-byte field plus its NUL");

// WHICH FLASH this is: "<git short hash>[-dirty]@<build time>", from
// tools/version.py as -D MEGAOPEN_BUILD. The version above is the same in every
// image, so it cannot tell two flashes apart in a log; this can.
//
// A function and not the macro itself: the define is handed to src/Version.cpp
// alone, because a define that changes every build would otherwise recompile
// every file and library every build. "unknown" when the script did not run.
namespace MegaOpen
{
    const char* buildId();
    // PlatformIO's env this image was built as ("t2can"): the GitHub update
    // fetches megaopen-<env>-<ver>.bin. Stamped the same way as buildId().
    const char* envName();
}
