#pragma once
#include "../Features.h"
#include <Arduino.h>
#include "../cfg/Config.h"

class WebUi;
class WifiManager;
#if FEATURE_KNOBS
namespace Knobs { class Service; }
#endif

// What net_svc runs (App::Housekeeping, Housekeeping.h stays the generic
// task): the web server's pass, WiFi with the log push and the address on the
// panel, the OTA trial's "a way in" test, and the heartbeat lines no task owns
// ([stk], [can2], [ble], [media], [hb], and the minute's [net]).
namespace App
{
    namespace NetHooks
    {
        // setup(), first: the shared objects the hooks read.
        void bind(Cfg::Config& cfg, WebUi& web, WifiManager& wifi);
#if FEATURE_KNOBS
        // The knob task, for its stack in [stk].
        void attachKnobs(const Knobs::Service* knobs);
#endif
        // setup(), after every attach: net_svc with these hooks.
        void start();

#if FEATURE_DISPLAY_CAN
        // The address as the panel shows it (the connect notice, the panel
        // menu's address row).
        void addressText(char* head, size_t hn, char* body, size_t bn);
#endif
#if FEATURE_LOG_PUSH
        // The status snapshot the log push sends (LogSetup::push).
        size_t logPushSnapshot(char* out, size_t cap);
#endif
    }
}
