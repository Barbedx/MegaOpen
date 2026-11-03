#pragma once
#include <stddef.h>
#include <stdint.h>

// Every change of the settings' STORED FORMAT, in one place. Config knows only
// today's NVS keys; what older firmware left behind is rewritten into them
// here, before Config reads a single key. tools/check_rules.py holds Config.cpp
// to it: a key Config reads, it also writes (no "read the old key if the new
// one is missing" in Config).
//
// WHEN a step is needed: a stored key is renamed, changes its type or its
// meaning. NOT for a new setting (Config::begin reads it with its default) and
// NOT for a changed default (change it in Config::begin and Settings.h; a
// stored value is the owner's and stays). When the owner can simply set it
// again on the page, prefer no carry-over at all.
//
// ROLLBACK: a new image runs on an OTA trial and may roll back to the release
// before it, which reads the NVS this step left. So a step writes the new key
// and KEEPS the old one; an old key is removed (drop()) only by a later step,
// once the release before CURRENT no longer reads it.
//
// HOW to add one:
//   1. Config (load and save) and the registry use the NEW key only.
//   2. Here: toV<N>(Store&) rewrites the old key into the new one, only when
//      the new one is absent, and drops keys no supported release reads. It
//      must be safe to run twice (a failed version write repeats it).
//   3. CURRENT = N, and `if (v < N) toV<N>(s);` in run(), after the others.
//   4. A case in test/test_migrations; a line in CHANGELOG.md.
// A step never touches the network settings (SSID, password, AP name, mDNS):
// a bad value there strands the board with no way in over the air.
//
// Each step runs once: "cfgVer" is the last step this board has run. The day
// no board older than a step is left, the step goes.
//
// Pure (no Arduino): Config hands it NVS through Store; host-tested.
namespace Cfg
{
    namespace Migrations
    {
        // The schema this firmware writes.
        inline constexpr uint8_t CURRENT = 4;

        // The few NVS calls a step makes (Config adapts Preferences).
        struct Store {
            virtual bool    has(const char* key) = 0;
            virtual bool    getBool(const char* key, bool def) = 0;
            virtual uint8_t getU8(const char* key, uint8_t def) = 0;
            // The stored text into out (cap incl. the NUL); "" when absent.
            virtual void    getStr(const char* key, char* out, size_t cap) = 0;
            virtual bool    putU8(const char* key, uint8_t v) = 0;
            virtual bool    putStr(const char* key, const char* v) = 0;
            virtual void    remove(const char* key) = 0;
            virtual ~Store() = default;
        };

        // From the stored "cfgVer" to CURRENT. Returns the version stored
        // after: CURRENT, or the old one when the version write failed (the
        // steps then run again next boot). `was` gets the version this boot
        // started from.
        uint8_t run(Store& s, uint8_t& was);
    }
}
