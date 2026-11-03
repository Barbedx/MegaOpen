#include "Migrations.h"
#include <initializer_list>
#include "../utils/Log.h"

namespace Cfg
{
    namespace Migrations
    {
        namespace
        {
            // Keys this firmware no longer reads (NO LEFTOVERS in the header).
            void drop(Store& s, std::initializer_list<const char*> keys)
            {
                for (const char* k : keys) if (s.has(k)) s.remove(k);
            }

            // v3 -> v4 (1.9): the panel role. 1.8's Bool "funcReg" (on = the
            // board registers = emulator) becomes "panelRole"
            // (display/PanelRole.h): carried over, because a wrong role after
            // the update fights the stock radio or shows nothing.
            // Dropped with it, everything older firmware left in NVS: the car
            // screen's index keys carSig0..2/carSigV (by name since 1.7), the
            // gear keys gr1..gr5 (unread since 1.6.2), and 1.6.x's tables
            // decode/huFmap/alerts/macros (in /cfg/car.json since 1.7).
            void toV4(Store& s)
            {
                if (s.has("funcReg") && !s.has("panelRole"))
                    s.putStr("panelRole", s.getBool("funcReg", true) ? "emulator" : "extension");
                drop(s, {"funcReg",
                         "carSig0", "carSig1", "carSig2", "carSigV",
                         "gr1", "gr2", "gr3", "gr4", "gr5",
                         "decode", "huFmap", "alerts", "macros"});
            }
        }

        uint8_t run(Store& s, uint8_t& was)
        {
            const uint8_t v = s.getU8("cfgVer", 0);
            was = v;
            // Every boot, so "did the migration run" is always answerable.
            LOGI("cfg", "schema v%u (current v%u)%s\n", (unsigned)v, (unsigned)CURRENT,
                 v >= CURRENT ? " — nothing to do" : " — migrating");
            if (v >= CURRENT) return v;

            // Steps in order, each from the version before it. v1..v3 changed
            // defaults only (those are Config::begin's now) and are gone: a
            // fresh board (no "cfgVer") runs the steps from here as well.
            if (v < 4) toV4(s);

            // An NVS write can fail (full namespace).
            if (s.putU8("cfgVer", CURRENT)) return CURRENT;
            LOGW("cfg", "SCHEMA WRITE FAILED — migration will re-run");
            return v;
        }
    }
}
