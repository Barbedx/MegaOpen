#include "../utils/LogBus.h"
#include "Config.h"
#include <nvs.h>

namespace Cfg
{
    static constexpr const char* NS = "megaopen";

    void Config::begin()
    {
        _p.begin(NS, false);

        // Read first, seed afterwards. The seed calls save(), which writes every
        // field; run before the reads it would blank values already in a
        // namespace without a `seeded` key (AffaDisplay's examples share the
        // `megaopen` namespace and the `ssid`/`pass` keys).
        _s.ssid  = _p.getString("ssid", "");
        _s.pass   = _p.getString("pass", "");
        if (!_s.ssid.length()) { _s.ssid = MO_WIFI_SSID; _s.pass = MO_WIFI_PASS; }
        _s.apName = _p.getString("apName", "MegaOpen");

        _s.mdnsName = _p.getString("mdns", "megaopen");
        _s.mdnsOn   = _p.isKey("mdnsOn") ? _p.getBool("mdnsOn", true) : true;
        _s.apPass = _p.getString("apPass", MO_AP_PASS);

        _s.timing.debounceMs     = _p.getUShort("dbMs",   20);
        _s.timing.fastDebounceMs = _p.getUShort("fdbMs",  2);
        _s.timing.longMs         = _p.getUShort("lngMs",  600);
        _s.timing.repeatMs       = _p.getUShort("repMs",  150);
        _s.timing.repeatEnabled  = _p.getBool  ("rep",    true);
        _s.encInverted           = _p.getBool  ("encInv", false);

        _s.displayType = _p.getString("dispType", "carminat");
        // AffaDisplay 2.0 has one UpdateList encoding; the "LCD" variant is gone.
        if (_s.displayType == "updatelist_menu") _s.displayType = "updatelist";
        _s.funcReg     = _p.getBool  ("funcReg", true);
        _s.bleMode     = _p.getString("bleMode", "ams");
        _s.autoTime    = _p.getBool  ("autoTime", true);
        _s.mediaScreen = _p.getBool  ("mediaScr", false);
        // isKey first: added after boards were seeded, and an absent key costs
        // an IDF error line per boot.
        _s.carScreen = _p.isKey("carScr") ? _p.getBool("carScr", false) : false;
        {
            static const char* const kSig[3] = {"carSig0", "carSig1", "carSig2"};
            for (uint8_t i = 0; i < 3; ++i) {
                const uint8_t v = _p.isKey(kSig[i]) ? _p.getUChar(kSig[i], _s.carSig[i]) : _s.carSig[i];
                _s.carSig[i] = v <= Vehicle::SIG_COUNT ? v : (uint8_t)Vehicle::SIG_COUNT;
            }
        }
        {
            const uint8_t v = _p.isKey("carPre") ? _p.getUChar("carPre", 0) : 0;
            _s.carPreset = v < (uint8_t)CarPreset::_Count ? v : 0;
        }

        _s.ancsPopup   = _p.getBool ("ancsPop", false);
        _s.mediaLayout = _p.getString("medLay", "fullscreen");

        _s.mediaStyle   = _p.getString("medSty", "classic");
        _s.mediaPane    = _p.getString("medPane", "off");
        // isKey first: an absent key costs an IDF error line per boot.
        _s.paneSpeed     = _p.isKey("paneSpd")  ? _p.getUChar ("paneSpd", 1)     : 1;
        _s.trackSplashMs = _p.isKey("splashMs") ? _p.getUShort("splashMs", 4000) : 4000;
        _s.paneCycleS    = _p.isKey("paneCyc")  ? _p.getUShort("paneCyc", 30)    : 30;
        _s.mediaSource  = _p.getString("medSrc", "ams");
        _s.huTitleCmd   = _p.getUChar ("huTtlCmd", 0);
        _s.huArtistCmd  = _p.getUChar ("huArtCmd", 0);
        _s.huTitleFirst = _p.getBool  ("huTtlFst", false);

        _s.keyPopup    = _p.getBool  ("keyPop",   true);
        _s.keyPopupMs  = _p.getUShort("keyPopMs", 2000);

        _s.routeUart   = _p.getUChar("rtUart", Keys::CLASS_ALL);
        _s.routeAms    = _p.getUChar("rtAms",  Keys::CLASS_TRANSPORT);
        _s.routeHid    = _p.getUChar("rtHid",  Keys::CLASS_VOLUME);
        _s.routeRes    = _p.getUChar("rtRes",  Keys::CLASS_NONE);
        _s.routeUsb    = _p.getUChar("rtUsb",  Keys::CLASS_TRANSPORT | Keys::CLASS_VOLUME);

        _s.uartEnabled = _p.getBool ("uartEn", true);
        _s.uartBaud    = _p.getULong("baud",   38400);
        _s.pulseMs     = _p.getUShort("pulse", 60);
        _s.resEnabled  = _p.getBool ("resEn",  false);

        _s.can2Bitrate  = _p.isKey("can2Bps") ? _p.getULong("can2Bps", 0) : 0;
        _s.can2Detected = _p.isKey("can2Det") ? _p.getULong("can2Det", 0) : 0;

        // Seed: every absent key makes the IDF print an error line at boot.
        // Everything above is stored or default, so this contradicts nothing.
        if (!_p.isKey("seeded")) {
            save();
            _p.putBool("seeded", true);
        }

        migrate();
    }

    // A changed default never reaches a seeded board; it needs a migration step.
    // Keep steps narrow: never touch network settings (SSID, password, AP name,
    // mDNS) — a bad value there strands the board with no way in over the air.
    void Config::migrate()
    {
        const uint8_t v = _p.getUChar("cfgVer", 0);
        const uint8_t kCurrent = 3;
        _verWas = v;
        _ver = v;
        // Printed every boot so "did the migration run" is always answerable.
        LogBus::printf("[cfg] schema v%u (current v%u)%s\n", (unsigned)v,
                      (unsigned)kCurrent, v >= kCurrent ? " — nothing to do" : " — migrating");
        if (v >= kCurrent) return;

        // v0 -> v1: the now-playing screen is on by default.
        if (v < 1) {
            _s.mediaScreen = true;
            _p.putBool("mediaScr", true);
        }

        // v1 -> v2: notifications become a popup, now-playing becomes fullscreen.
        if (v < 2) {
            _s.ancsPopup = true;
            _p.putBool("ancsPop", true);
            _s.mediaLayout = "fullscreen";
            _p.putString("medLay", "fullscreen");
        }

        // v2 -> v3 moved a stored CTX pin back to the board's; the CAN pins are
        // no longer settings, so it has nothing left to do.

        // An NVS write can fail (full namespace); Preferences reports it only
        // in the return value.
        const size_t wrote = _p.putUChar("cfgVer", kCurrent);
        _ver = wrote ? kCurrent : v;
        if (!wrote) LogBus::line("[cfg] SCHEMA WRITE FAILED — migration will re-run");
    }

    void Config::save()
    {
        Guard g(*this);
        _p.putString("ssid", _s.ssid);
        _p.putString("pass", _s.pass);
        _p.putString("apName", _s.apName);

        _p.putString("mdns", _s.mdnsName);
        _p.putBool  ("mdnsOn", _s.mdnsOn);
        _p.putString("apPass", _s.apPass);

        _p.putUShort("dbMs",  _s.timing.debounceMs);
        _p.putUShort("fdbMs", _s.timing.fastDebounceMs);
        _p.putUShort("lngMs", _s.timing.longMs);
        _p.putUShort("repMs", _s.timing.repeatMs);
        _p.putBool  ("rep",   _s.timing.repeatEnabled);
        _p.putBool  ("encInv", _s.encInverted);

        _p.putString("dispType", _s.displayType);
        _p.putBool  ("funcReg",  _s.funcReg);

        _p.putString("bleMode",  _s.bleMode);
        _p.putBool  ("autoTime", _s.autoTime);
        _p.putBool  ("mediaScr", _s.mediaScreen);
        _p.putBool  ("carScr",   _s.carScreen);
        _p.putUChar ("carSig0",  _s.carSig[0]);
        _p.putUChar ("carSig1",  _s.carSig[1]);
        _p.putUChar ("carSig2",  _s.carSig[2]);
        _p.putUChar ("carPre",   _s.carPreset);

        _p.putBool  ("ancsPop", _s.ancsPopup);
        _p.putString("medLay",  _s.mediaLayout);

        _p.putString("medSty",   _s.mediaStyle);
        _p.putString("medPane",  _s.mediaPane);
        _p.putUChar ("paneSpd",  _s.paneSpeed);
        _p.putUShort("splashMs", _s.trackSplashMs);
        _p.putUShort("paneCyc",  _s.paneCycleS);
        _p.putString("medSrc",   _s.mediaSource);
        _p.putUChar ("huTtlCmd", _s.huTitleCmd);
        _p.putUChar ("huArtCmd", _s.huArtistCmd);
        _p.putBool  ("huTtlFst", _s.huTitleFirst);

        _p.putBool  ("keyPop",   _s.keyPopup);
        _p.putUShort("keyPopMs", _s.keyPopupMs);

        _p.putUChar ("rtUart", _s.routeUart);
        _p.putUChar ("rtAms",  _s.routeAms);
        _p.putUChar ("rtHid",  _s.routeHid);
        _p.putUChar ("rtRes",  _s.routeRes);
        _p.putUChar ("rtUsb",  _s.routeUsb);

        _p.putBool  ("uartEn", _s.uartEnabled);
        _p.putULong ("baud",   _s.uartBaud);
        _p.putUShort("pulse",  _s.pulseMs);
        _p.putBool  ("resEn",  _s.resEnabled);
        _p.putULong ("can2Bps", _s.can2Bitrate);
        _p.putULong ("can2Det", _s.can2Detected);
        _gen.fetch_add(1, std::memory_order_release);
    }

    bool Config::trySnapshot(Settings& out)
    {
        if (!tryLock()) return false;
        out = _s;
        unlock();
        return true;
    }

    void Config::saveCan2Detected(uint32_t bps)
    {
        // No lock: one aligned word, and net_svc must not wait behind a web
        // request. A save() racing it stores the old or the new rate.
        _s.can2Detected = bps;
        _p.putULong("can2Det", bps);
    }

    void Config::saveKeyMap(const Keys::KeyMap& m)
    {
        uint8_t blob[Keys::KeyMap::BLOB_SIZE];
        m.toBlob(blob);
        _p.putBytes("keymap", blob, sizeof(blob));
    }

    void Config::loadKeyMap(Keys::KeyMap& m)
    {
        uint8_t blob[Keys::KeyMap::BLOB_SIZE];
        size_t n = _p.getBytes("keymap", blob, sizeof(blob));
        if (n != sizeof(blob) || !m.fromBlob(blob, sizeof(blob))) m.loadDefaults();
    }

#if FEATURE_DISPLAY_CAN
    void Config::saveAffaMap(const Keys::AffaMap& m)
    {
        uint8_t blob[Keys::AffaMap::BLOB_SIZE];
        m.toBlob(blob);
        _p.putBytes("affamap", blob, sizeof(blob));
    }

    void Config::loadAffaMap(Keys::AffaMap& m)
    {
        uint8_t blob[Keys::AffaMap::BLOB_SIZE];
        size_t n = _p.getBytes("affamap", blob, sizeof(blob));
        if (n != sizeof(blob) || !m.fromBlob(blob, sizeof(blob))) m.loadDefaults();
    }

    bool Config::savePaneBits(const uint8_t* b, size_t n)
    {
        return _p.putBytes("paneBits", b, n) == n;
    }

    bool Config::loadPaneBits(uint8_t* b, size_t n)
    {
        // isKey first: getBytes on an absent key prints an IDF error per boot.
        if (!_p.isKey("paneBits") || _p.getBytesLength("paneBits") != n) return false;
        return _p.getBytes("paneBits", b, n) == n;
    }
#endif

    void Config::saveTuning(const Canbox::Tuning& t)
    {
        uint8_t blob[Canbox::Tuning::BLOB_SIZE];
        t.toBlob(blob);
        _p.putBytes("rav4tune", blob, sizeof(blob));
    }

    void Config::loadTuning(Canbox::Tuning& t)
    {
        uint8_t blob[Canbox::Tuning::BLOB_SIZE];
        size_t n = _p.getBytes("rav4tune", blob, sizeof(blob));
        // Short read or unknown layout: keep the defaults.
        if (n != sizeof(blob) || !t.fromBlob(blob, sizeof(blob))) t = Canbox::Tuning{};
    }

    bool Config::saveDecode(const uint8_t* b, size_t n)
    {
        return _p.putBytes("decode", b, n) == n;
    }

    size_t Config::loadDecode(uint8_t* b, size_t cap)
    {
        // isKey first: getBytes on an absent key prints an IDF error per boot.
        if (!_p.isKey("decode")) return 0;
        const size_t n = _p.getBytesLength("decode");
        if (!n || n > cap) return 0;
        return _p.getBytes("decode", b, n);
    }

    bool Config::eraseDecode()
    {
        return !_p.isKey("decode") || _p.remove("decode");
    }

    bool Config::saveAlerts(const Vehicle::AlertRules& a)
    {
        uint8_t blob[Vehicle::AlertRules::BLOB_BYTES];
        const size_t n = a.toBlob(blob);
        return _p.putBytes("alerts", blob, n) == n;
    }

    bool Config::loadAlerts(Vehicle::AlertRules& a)
    {
        if (!_p.isKey("alerts")) return false;
        uint8_t blob[Vehicle::AlertRules::BLOB_BYTES];
        if (_p.getBytesLength("alerts") != sizeof(blob)) return false;
        const size_t n = _p.getBytes("alerts", blob, sizeof(blob));
        return a.fromBlob(blob, n);
    }

    int Config::nvsFreeEntries()
    {
        nvs_stats_t st;
        return nvs_get_stats(nullptr, &st) == ESP_OK ? (int)st.free_entries : -1;
    }

    void Config::saveMacros(const Keys::MacroEngine& m)
    {
        uint8_t blob[Keys::MacroEngine::BLOB_SIZE];
        m.toBlob(blob);
        _p.putBytes("macros", blob, sizeof(blob));
    }

    void Config::saveMacroBlob(const uint8_t (&blob)[Keys::MacroEngine::BLOB_SIZE])
    {
        _p.putBytes("macros", blob, sizeof(blob));
    }

    void Config::loadMacros(Keys::MacroEngine& m)
    {
        uint8_t blob[Keys::MacroEngine::BLOB_SIZE];
        size_t n = _p.getBytes("macros", blob, sizeof(blob));
        // Defaults (two disabled examples) on any doubt; a half-restored table
        // would send frames nobody chose to the head unit.
        if (n != sizeof(blob) || !m.fromBlob(blob, sizeof(blob))) m.loadDefaults();
    }

#if FEATURE_RESISTIVE_KEYS
    void Config::saveResMap(const Sinks::ResistiveMap& m)
    {
        uint8_t blob[Sinks::ResistiveMap::BLOB_SIZE];
        m.toBlob(blob);
        _p.putBytes("resmap", blob, sizeof(blob));
    }

    void Config::loadResMap(Sinks::ResistiveMap& m)
    {
        uint8_t blob[Sinks::ResistiveMap::BLOB_SIZE];
        size_t n = _p.getBytes("resmap", blob, sizeof(blob));
        // Defaults (every rung unassigned) on any doubt.
        if (n != sizeof(blob) || !m.fromBlob(blob, sizeof(blob))) m.loadDefaults();
    }
#endif

#if FEATURE_KNOBS
    void Config::saveKnobs(const Knobs::Config& k)
    {
        uint8_t blob[Knobs::Config::BLOB_SIZE];
        k.toBlob(blob);
        _p.putBytes("knobs", blob, sizeof(blob));
    }

    void Config::loadKnobs(Knobs::Config& k)
    {
        uint8_t blob[Knobs::Config::BLOB_SIZE];
        // isKey first: getBytes on an absent key prints an IDF error per boot.
        const size_t n = _p.isKey("knobs") ? _p.getBytes("knobs", blob, sizeof(blob)) : 0;
        // Defaults (everything off) on any doubt. The length goes to
        // fromBlob(): a v3 blob is shorter than v4 and still valid.
        if (!n || n > sizeof(blob) || !k.fromBlob(blob, (uint8_t)n)) k = Knobs::Config{};
    }
#endif

    void Config::factoryReset()
    {
        Guard g(*this);
        _p.clear();
        _s = Settings{};
        _gen.fetch_add(1, std::memory_order_release);
    }
}
