#pragma once
#include <stddef.h>
#include <stdint.h>

// GET /api/catalog (docs/design/web-ui.md, "/api/catalog shape"): every list the
// page renders instead of hard-coding — layouts, styles, scenes, actions, key
// codes, panel keys, console commands, enums. Each list is read from the table
// that owns it (MediaScreen, MediaPane, KeyAction, AffaMap, Rav4Catalog, ...),
// never copied here; the few with no other owner are declared in Catalog.cpp.
//
// Pure (no Arduino): env:native writes build/catalog.json from this same code,
// and tools/build_sim.py serves that file as the simulator's catalog.
namespace Catalog
{
    // Where the JSON goes: the httpd chunk sender on the board, a std::string
    // in the native test. Called with short pieces, many times.
    struct Out {
        virtual void put(const char* s, size_t n) = 0;
    };

    // The whole document. Depends on the build's FEATURE_* flags only.
    void write(Out& out);

    // `features`: which optional subsystems this image has, one bool each, so
    // the page fetches only routes that exist and shows only cards that work.
    // `macro` names the FEATURE_* switch it is, for tools/build_sim.py, which
    // turns each board env's flags (build/features/<env>.json, written by
    // tools/gen_pages.py during that env's build) into the simulator's env.
    struct Feature { const char* id; const char* macro; bool on; };
    extern const Feature FEATURES[];
    extern const size_t FEATURE_COUNT;

    // POST /api/settings validates bleMode against the same rows it serves.
    bool bleModeKnown(const char* id);
    // POST /api/display validates the panel type likewise (display builds).
    bool panelKnown(const char* id);
    // POST /api/media and /api/display/menu validate the scroll byte against
    // the `arrows` enum it serves (display builds).
    bool arrowsKnown(int v);
}
