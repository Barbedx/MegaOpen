#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "../Features.h"
#include "../keys/KeyMap.h"
#if FEATURE_DISPLAY_CAN
// AffaMap.h pulls in AffaDisplay.h; a build without the panel must not.
#include "../keys/AffaMap.h"
#endif
#include "../proto/canbox/Rav4Emitter.h"
#include "Secrets.h"
#if FEATURE_RESISTIVE_KEYS
#include "../sinks/ResistiveMap.h"
#endif
#if FEATURE_KNOBS
#include "../knobs/KnobConfig.h"
#endif
#include "../input/KeyDebouncer.h"
#include "../board/BoardProfile.h"
#include "../keys/KeyAction.h"
#include "../keys/MacroEngine.h"

// Everything the user can change, in NVS. Small values only; the CAN decode
// table goes to the filesystem as JSON (ARCHITECTURE.md §7).
namespace Cfg
{
    struct Settings {
        // network
        String   ssid = MO_WIFI_SSID;      // defaults: src/secrets.example.h
        String   pass = MO_WIFI_PASS;
        String   mdnsName = "megaopen";     // -> http://megaopen.local
        // Off = no .local name; see WifiManager::startMdns().
        bool     mdnsOn   = true;
        String   apName = "MegaOpen";
        String   apPass = MO_AP_PASS;      // >= 8 chars or the AP comes up open

        // input timing
        Input::KeyDebouncer::Params timing;
        bool     encInverted = false;

        // OEM display
        String   displayType = "carminat";   // carminat | updatelist | updatelist_menu
        bool     funcReg     = true;         // we are the master: run FUNCSREG ourselves

        // BLE role: off | ams (phone: now-playing + clock) | keyboard (HID to the
        // head unit) | both (transport to the phone, volume to the head unit).
        // Read once at boot; "off" keeps ~40 kB of host state out of RAM.
        String   bleMode = "ams";
        // No wifiSleep setting: WiFi power save is derived from bleMode
        // (WifiManager::sleepRequired()). BT on with power save off makes the
        // IDF WiFi driver abort at radio start and boot-loop.

        // Push the phone's clock to the panel. The panel's own clock is not
        // drawn inside the menu/media screen, so we compose one there.
        bool     autoTime = true;

        // Show the now-playing screen. Persisted; Config::migrate() turns it on
        // once for boards seeded before it defaulted on.
        bool     mediaScreen = true;

        // Three-row primitive for now-playing: "fullscreen" (0x21 mode 0x05) or
        // "menu" (mode 0x01). Persisted because leaving fullscreen needs an
        // explicit hide; a forgotten fullscreen swallows every later setText
        // while returning Ok (AffaDisplay WIRE-SPEC 8.6 [CAP]).
        String   mediaLayout = "fullscreen";

        // Text style, stored by name (see MediaScreen::Style).
        String   mediaStyle = "classic";

        // The 48x48 graphic layer (see MediaPane); covered under "menu"/"fullscreen".
        // Off by default for bus budget: 304 wire bytes (~44 ACKed CAN frames)
        // vs 14 for a text screen (see the BUS_OFF circuit breaker in main.cpp).
        // Stored by name; "auto" = carousel, paneCycleS apart.
        String   mediaPane = "off";

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
        String   mediaSource = "ams";

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
        uint8_t  routeAms  = Keys::CLASS_TRANSPORT | Keys::CLASS_PHONE;
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

    class Config {
    public:
        void begin();                      // load or seed defaults
        void save();                       // settings only
        void saveKeyMap(const Keys::KeyMap& m);
        void loadKeyMap(Keys::KeyMap& m);  // falls back to defaults when absent
#if FEATURE_DISPLAY_CAN
        void saveAffaMap(const Keys::AffaMap& m);
        void loadAffaMap(Keys::AffaMap& m);
        // The pane's Custom picture (MediaPane::BYTES). load: false, and `b`
        // untouched, when none was saved or its size differs.
        bool savePaneBits(const uint8_t* b, size_t n);   // false: not written
        bool loadPaneBits(uint8_t* b, size_t n);
#endif
        // The uncertain-value block, resolved in a car, so it must survive reboots.
        void saveTuning(const Canbox::Tuning& t);
        void loadTuning(Canbox::Tuning& t);
        // "Send car data to the head unit": off until the owner first turns
        // it on, then remembered (2026-10-20, after doors, lights, indicators
        // and steering were checked in the car).
        void saveHuSend(bool on) { _p.putBool("huSend", on); }
        bool loadHuSend()        { return _p.getBool("huSend", false); }
        // The user's decode map (Vehicle::DecodeTable::pack), key "decode".
        // load: its length, 0 when there is none or it does not fit `cap`.
        // erase: back to factory (decode_defaults.inc). False: not written.
        bool   saveDecode(const uint8_t* b, size_t n);
        size_t loadDecode(uint8_t* b, size_t cap);
        bool   eraseDecode();
        // NVS entries left (32 B each), for the boot line. -1: unknown.
        int    nvsFreeEntries();
        // Not feature-gated: effects this build lacks are refused at execution.
        void saveMacros(const Keys::MacroEngine& m);
        // The web's staged table, serialised (MacroEngine::toBlob) as edited,
        // before the keys task applies it. The web makes the blob under the
        // staging mutex and writes it to flash after.
        void saveMacroBlob(const uint8_t (&blob)[Keys::MacroEngine::BLOB_SIZE]);
        void loadMacros(Keys::MacroEngine& m);
#if FEATURE_RESISTIVE_KEYS
        // Ladder and per-action rungs in one blob: a rung is only meaningful
        // with the ladder that gives it its ohms.
        void saveResMap(const Sinks::ResistiveMap& m);
        void loadResMap(Sinks::ResistiveMap& m);
#endif
#if FEATURE_KNOBS
        // Knobs and their backlight, one blob.
        void saveKnobs(const Knobs::Config& k);
        void loadKnobs(Knobs::Config& k);
#endif
        void factoryReset();

        // net_svc: the detected CAN A rate, only that key. Not save(): one NVS
        // write, and nothing else in Settings is touched from that task.
        void saveCan2Detected(uint32_t bps);

        Settings& s() { return _s; }

        // Stored schema version, and what migrate() started from on this boot.
        uint8_t version() const { return _ver; }
        uint8_t migratedFrom() const { return _verWas; }

    private:
        // One-time, versioned fix-ups for older defaults. End of begin(); never
        // touches network settings (see the definition).
        void migrate();

        uint8_t     _ver = 0;      // what is stored now, after migrate()
        uint8_t     _verWas = 0;   // what was stored when this boot started
        Preferences _p;
        Settings    _s;
    };
}
