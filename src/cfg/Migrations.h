#pragma once
#include <Preferences.h>
#include <stdint.h>

// Every change of the settings' STORED FORMAT, in one place. Config knows only
// today's NVS keys; what older firmware left behind is rewritten into them
// here, before Config reads a single key, and the old keys are removed.
// tools/check_rules.py holds Config.cpp to it: a key Config reads, it also
// writes (no "read the old key if the new one is missing" in Config).
//
// WHEN a step is needed: a stored key is renamed, changes its type or its
// meaning, or is no longer read. NOT for a new setting (Config::begin reads
// it with its default) and NOT for a changed default (change it in
// Config::begin and Settings.h; a stored value is the owner's and stays).
// When the owner can simply set it again on the page, prefer no carry-over:
// drop the old key only.
//
// HOW to add one:
//   1. Config (load and save) and the registry use the NEW key only.
//   2. Here: a function toV<N>(Preferences&) that rewrites the old key into
//      the new one and removes the old (drop() for keys merely gone). It
//      must be safe to run twice (a failed version write repeats it).
//   3. CURRENT = N, and `if (v < N) toV<N>(p);` in run(), after the others.
//   4. A line in CHANGELOG.md: which key, from which version.
// A step never touches the network settings (SSID, password, AP name, mDNS):
// a bad value there strands the board with no way in over the air.
//
// Each step runs once: NVS "cfgVer" is the last step this board has run. The
// day no board older than a step is left, the step goes; with every step gone
// this file is only the version line.
namespace Cfg
{
    namespace Migrations
    {
        // The schema this firmware writes.
        inline constexpr uint8_t CURRENT = 4;

        // From the stored "cfgVer" to CURRENT, logged ([cfg]). Returns the
        // version stored after: CURRENT, or the old one when the version
        // write failed (the steps then run again next boot). `was` gets the
        // version this boot started from.
        uint8_t run(Preferences& p, uint8_t& was);
    }
}
