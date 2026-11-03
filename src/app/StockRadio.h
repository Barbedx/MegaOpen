#pragma once
#include "../Features.h"
#if FEATURE_DISPLAY_CAN
#include <stdint.h>
#include "../cfg/Config.h"

// The stock radio beside us: the Extension role's own work
// (display/PanelRole.h). Today that is the radio's code (display/RadioCode.h):
// after the battery was off the radio asks for its four digits on the panel,
// and the board types them from the setting `radioCode` as the stalk would.
//
// On the panel task only. begin() from setup(), after App::Panel::begin()
// (the keys leave through the library's pressKey); onRx() from the link's
// raw tap inside poll(); step() after every pump. Only the Extension role
// calls begin(): before it, active() is false and every call is a no-op.
namespace App
{
    namespace StockRadio
    {
        // The setting is read here and again on the panel task whenever the
        // page saves (Config::generation(), tryLock: never blocking).
        void begin(Cfg::Config& cfg);
        // begin() was called: this board extends a stock radio.
        bool active();

        // A frame the controller received on the panel bus (never our own).
        void onRx(uint32_t id, const uint8_t* d, uint8_t len, uint32_t nowMs);
        // The next key when due, the verdict when it is time, both to the
        // log (tag `code`).
        void step(uint32_t nowMs);
    }
}
#endif
