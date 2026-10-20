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
#include "Settings.h"

// Everything the user can change, in NVS. Small values only; the CAN decode
// table goes to the filesystem as JSON (ARCHITECTURE.md §7).
namespace Cfg
{
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
