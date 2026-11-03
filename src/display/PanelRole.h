#pragma once
#include <stdint.h>
#include <string.h>

// How this board sits on the panel bus: the owner's choice, never the
// board's. The setting `panelRole` (Плата → Система → «Роль на шині щитка»,
// NVS, read once at boot: a reboot applies it) names one of two ways of
// connecting the board, and everything the choice decides reads the role,
// not the bus:
//
//   Emulator  — there is no stock radio. The board IS the radio: it syncs,
//               registers its functions (FUNCSREG), ACKs, draws every screen
//               and gets the panel's keys. (Up to 1.8: «Реєструвати щиток» on.)
//   Extension — the stock radio stays and owns the bus. The board never
//               syncs, registers or ACKs (AffaDisplay's passive mode): it
//               listens, draws over the radio's screens, injects keys, and
//               types the radio's code when the radio asks (app/StockRadio).
//               (Up to 1.8: «Реєструвати щиток» off.)
//
// The role is not detected and never switched by what the bus says. A stock
// radio syncing beside an Emulator, or no radio beside an Extension, is
// reported — health `panel`, a [panel] line (main.cpp panelWatchStep) — and
// nothing more: the board does what its role says. App::Panel::role() is the
// boot copy every task reads; Settings.panelRole is what the page saves for
// the next boot.
namespace MegaOpen
{
    enum class PanelRole : uint8_t { Emulator = 0, Extension = 1 };

    namespace PanelRoles
    {
        inline constexpr uint8_t COUNT = 2;

        // The ids, as stored, sent and logged; the page's words: k.role.<id>.
        inline const char* id(uint8_t i)
        {
            return i == (uint8_t)PanelRole::Extension ? "extension" : "emulator";
        }
        inline const char* id(PanelRole r) { return id((uint8_t)r); }

        // An id back to its role; false (out untouched) for anything else.
        inline bool parse(const char* s, PanelRole& out)
        {
            if (!s) return false;
            for (uint8_t i = 0; i < COUNT; ++i)
                if (!strcmp(s, id(i))) { out = (PanelRole)i; return true; }
            return false;
        }
    }
}
