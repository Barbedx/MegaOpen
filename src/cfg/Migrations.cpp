#include "Migrations.h"
#include <initializer_list>
#include "../utils/LogBus.h"

namespace Cfg
{
    namespace Migrations
    {
        namespace
        {
            // Keys no firmware reads any more: removed, so they stop taking NVS
            // entries. Nothing is carried over from them (a setting they held
            // is set again on the page).
            void drop(Preferences& p, std::initializer_list<const char*> keys)
            {
                for (const char* k : keys) if (p.isKey(k)) p.remove(k);
            }

            // v3 -> v4 (1.9): the panel role. 1.8's Bool "funcReg" (on = the
            // board registers = emulator) becomes "panelRole"
            // (display/PanelRole.h): carried over, because a wrong role after
            // the update fights the stock radio or shows nothing. Dead since
            // 1.6/1.7: the car screen's index rows (carSig*, now carSigN*) and
            // 1.6.1's gear keys (gr*, now g*).
            void toV4(Preferences& p)
            {
                if (p.isKey("funcReg")) {
                    if (!p.isKey("panelRole"))
                        p.putString("panelRole", p.getBool("funcReg", true) ? "emulator" : "extension");
                    p.remove("funcReg");
                }
                drop(p, {"carSig0", "carSig1", "carSig2", "carSigV", "gr1", "gr2", "gr3", "gr4", "gr5"});
            }
        }

        uint8_t run(Preferences& p, uint8_t& was)
        {
            const uint8_t v = p.getUChar("cfgVer", 0);
            was = v;
            // Every boot, so "did the migration run" is always answerable.
            LOGI("cfg", "schema v%u (current v%u)%s\n", (unsigned)v, (unsigned)CURRENT,
                 v >= CURRENT ? " — nothing to do" : " — migrating");
            if (v >= CURRENT) return v;

            // Steps in order, each from the version before it. v1..v3 changed
            // defaults only (those are Config::begin's now) and are gone: a
            // fresh board (no "cfgVer") runs the steps from here as well.
            if (v < 4) toV4(p);

            // An NVS write can fail (full namespace); Preferences reports it
            // only in the return value.
            if (p.putUChar("cfgVer", CURRENT)) return CURRENT;
            LOGW("cfg", "SCHEMA WRITE FAILED — migration will re-run");
            return v;
        }
    }
}
