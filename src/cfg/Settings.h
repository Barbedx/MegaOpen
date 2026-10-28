#pragma once
// Cfg::Settings on its own, without Preferences or Arduino, so the settings
// registry and its native tests see the very struct Config stores.
#include <stdint.h>
#include "Secrets.h"
#include "../input/KeyDebouncer.h"
#include "../keys/KeyAction.h"
#include "../vehicle/Signals.h"
#ifdef NATIVE
#include <string>
#else
#include <Arduino.h>
#endif

namespace Cfg
{
    // Arduino's String on the board; the host build has none.
#ifdef NATIVE
    using Str = std::string;
#else
    using Str = String;
#endif

    // The car screen's rows: the three chosen signals, or the trip computer
    // (consumption now/average, range, trip distance; MenuUi::tripRows).
    // Stored as the number, so new presets are appended.
    enum class CarPreset : uint8_t { Custom = 0, Trip, _Count };

    struct Settings {
        // network
        Str      ssid = MO_WIFI_SSID;      // defaults: src/secrets.example.h
        Str      pass = MO_WIFI_PASS;
        Str      mdnsName = "megaopen";     // -> http://megaopen.local
        // Off = no .local name; see WifiManager::startMdns().
        bool     mdnsOn   = true;
        // WiFi off while MegaOpen Companion is on the USB link (src/net/WifiManager.h):
        // the app carries the page, the alerts and the internet then.
        bool     wifiAuto = false;
        // The driving reminders' speeds, km/h (src/vehicle/Derived.h); 0 = off.
        uint8_t  remindLightsKmh = 10;
        uint8_t  remindBrakeKmh  = 5;
        // The gears, km/h per 1000 rpm x10, 1st..6th (src/vehicle/Derived.h);
        // 0 = no such gear. Copy what [gear] learns, or the live gear_ratio.
        // The log (docs/design/logging.md): the global threshold, per-tag
        // thresholds ("hu=TRC knobs=DBG"), and the lowest level each sink takes.
        // LogLevel values: 0 ERR .. 4 TRC.
        uint8_t  logLevel  = 2;
        Str      logTags   = "";
        uint8_t  logWeb    = 4;
        uint8_t  logFlash  = 3;
        uint8_t  logPush   = 4;
        uint8_t  logSerial = 2;
        uint16_t gearRatio[6] = {80, 147, 207, 275, 359, 431};
        Str      apName = "MegaOpen";
        Str      apPass = MO_AP_PASS;      // >= 8 chars or the AP comes up open

        // input timing
        Input::KeyDebouncer::Params timing;
        bool     encInverted = false;

        // OEM display
        Str      displayType = "carminat";   // carminat | updatelist
        bool     funcReg     = true;         // we are the master: run FUNCSREG ourselves
        // The page's developer mode, told to the board: RAM only, never saved,
        // off after every boot. Shows the page's developer tools.
        bool     devMode     = false;

        // BLE role: off | ams (phone: now-playing + clock) | keyboard (HID to the
        // head unit) | both (transport to the phone, volume to the head unit).
        // Read once at boot; "off" keeps ~40 kB of host state out of RAM.
        Str      bleMode = "ams";
        // No wifiSleep setting: WiFi power save is derived from bleMode
        // (WifiManager::sleepRequired()). BT on with power save off makes the
        // IDF WiFi driver abort at radio start and boot-loop.

        // Push the phone's clock to the panel. The panel's own clock is not
        // drawn inside the menu/media screen, so we compose one there.
        bool     autoTime = true;

        // Show the now-playing screen. Persisted; Config::migrate() turns it on
        // once for boards seeded before it defaulted on.
        bool     mediaScreen = true;

        // The car screen: three signals instead of now-playing, one per row
        // of a fullscreen. carSig holds Vehicle::Sig ids, SIG_COUNT = an empty
        // row; ids are stored, which is safe because signals are only appended.
        bool     carScreen = false;
        uint8_t  carSig[3] = {Vehicle::SIG_ENG_TEMP, Vehicle::SIG_BATT_V, Vehicle::SIG_FUEL_LEVEL};
        // What the rows are: a CarPreset (0 = carSig, 1 = the trip rows).
        uint8_t  carPreset = 0;

        // Three-row primitive for now-playing: "fullscreen" (0x21 mode 0x05) or
        // "menu" (mode 0x01). Persisted because leaving fullscreen needs an
        // explicit hide; a forgotten fullscreen swallows every later setText
        // while returning Ok (AffaDisplay WIRE-SPEC 8.6 [CAP]).
        Str      mediaLayout = "fullscreen";

        // Text style, stored by name (see MediaScreen::Style).
        Str      mediaStyle = "classic";

        // The 48x48 graphic layer (see MediaPane); covered under "menu"/"fullscreen".
        // Off by default for bus budget: 304 wire bytes (~44 ACKed CAN frames)
        // vs 14 for a text screen (see the BUS_OFF circuit breaker in main.cpp).
        // Stored by name; "auto" = carousel, paneCycleS apart.
        Str      mediaPane = "off";

        // Seconds per carousel stop. WebUi floors it at 5.
        uint16_t paneCycleS = 30;

        // Animation steps per delivered image; the pane is bus-bound at ~2
        // images/s (measured 21-24 per 10 s).
        uint8_t  paneSpeed = 1;

        // After a title change, a full-width title/artist/album fullscreen for
        // this long (the screen beside the pane is only 8 columns). 0 = off.
        uint16_t trackSplashMs = 4000;

        // Now-playing source: "ams" (iPhone), "hu" (Raise UART text) or "auto"
        // (head unit while live, else phone). "ams" by default: the head unit's
        // text frame is undocumented and learned at runtime.
        Str      mediaSource = "ams";

        // Raise command bytes carrying title and artist. 0 = learn it (see
        // HuMediaSource). Persisted: found once, in a car.
        uint8_t  huTitleCmd  = 0;
        uint8_t  huArtistCmd = 0;
        // "Title - Artist" order when one command carries both.
        bool     huTitleFirst = false;

        // ---- the key popup ---------------------------------------------------
        // Flash the pressed key and what the head unit was told.
        bool     keyPopup   = true;
        uint16_t keyPopupMs = 2000;

        // iPhone notifications as a panel popup. Off by default: a popup covers
        // now-playing. Notifications are captured in the web portal either way.
        bool     ancsPopup = false;

        // ---- key routing ---------------------------------------------------
        // Class mask per destination (Keys::CLASS_*). A key may go to several;
        // transport routed to both head unit and phone skips two tracks.
        // Defaults: head unit takes everything, phone takes transport and calls,
        // HID takes volume. UART overlapping AMS on transport is deliberate.
        uint8_t  routeUart = Keys::CLASS_ALL;
        uint8_t  routeAms  = Keys::CLASS_TRANSPORT;
        uint8_t  routeHid  = Keys::CLASS_VOLUME;
        // KEY1/KEY2: none by default, for safety. It puts a real voltage on a
        // wire into the head unit; an untaught rung is a phantom button press.
        uint8_t  routeRes  = Keys::CLASS_NONE;
        // The wired keyboard (FEATURE_USB_HID): the phone has AMS for transport,
        // the head unit's USB takes both.
        uint8_t  routeUsb  = Keys::CLASS_TRANSPORT | Keys::CLASS_VOLUME;

        // head-unit link
        bool     uartEnabled = true;
        uint32_t uartBaud    = 38400;
        uint16_t pulseMs     = 60;         // encoder press -> release spacing

        // KEY1/KEY2 link. Off by default, as routeRes.
        bool     resEnabled  = false;

        // CAN A, the car's bus on the T-2CAN (Mcp2518Source; Bus::Autobaud).
        // can2Bitrate: a rate set by hand, which always wins; 0 = find it.
        // can2Detected: the rate the search last locked on, tried first at the
        // next boot. Written by net_svc when the car task reports a new one,
        // never by the web. Both are 0 or one of 125k/250k/500k/1M.
        uint32_t can2Bitrate  = 0;
        uint32_t can2Detected = 0;
    };
}
