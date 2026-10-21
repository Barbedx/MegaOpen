#pragma once
#include <stdint.h>

// Where the panel's clock comes from, and the health chain `clock` built on
// it. Two automatic sources: the phone (CTS, while it is connected) and the
// head unit's 0x9D (once a minute, unprompted; src/main.cpp onHuClock). Either
// one alone keeps the panel's clock right. Pure: native-tested (test_media).
namespace App
{
    namespace Ui
    {
        // Who set the panel's clock last (GET /api/media clockSrc).
        enum class ClockSrc : uint8_t { None = 0, Hu, Cts, Manual };

        // The head unit sends 0x9D every minute: one missed is fine, three
        // missed is a head unit that stopped (or never was).
        inline constexpr uint32_t HU_CLOCK_FRESH_MS = 150000;

        struct ClockHealth {
            const char* state;   // "off" | "idle" | "ok"
            const char* code;    // stable, the page keys its text on it
            const char* why;     // English, for the log and the API
        };

        // want: autoTime is on. phone: the AMS/CTS link is up. src, sinceSetMs:
        // who set the clock last and how long ago. The phone counts while it is
        // connected (it sets the time every minute it changes); the head unit
        // while its minutes keep coming. A manual set is neither.
        inline ClockHealth clockHealth(bool want, bool phone, ClockSrc src, uint32_t sinceSetMs)
        {
            if (!want) return {"off", "clock_off", "autoTime is off"};
            if (src == ClockSrc::Cts && phone) return {"ok", "clock_phone", "from the phone (CTS)"};
            if (src == ClockSrc::Hu && sinceSetMs <= HU_CLOCK_FRESH_MS)
                return {"ok", "clock_hu", "from the head unit (0x9D)"};
            if (phone) return {"idle", "clock_waiting", "phone connected, no time received yet"};
            return {"idle", "clock_no_source", "no phone, and no clock from the head unit"};
        }
    }
}
