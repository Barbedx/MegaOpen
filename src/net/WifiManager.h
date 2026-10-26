#pragma once
#include <Arduino.h>
#include "../cfg/Config.h"

// STA if credentials exist and associate within the timeout, otherwise an AP,
// so the module is always reachable from a phone.
//
// NOTHING HERE WAITS. begin() and loop() run on the loop task, which also owns
// the panel and the keys: the old 15 s boot wait and the 8 s retry every minute
// froze both, dropped the panel's session and delivered keys seconds late. A
// connection is now a phase that loop() polls (Scanning -> Joining -> Idle).
class WifiManager {
public:
    void begin(Cfg::Config& cfg);
    void loop(uint32_t nowMs);

    bool isSta() const { return _sta; }
    // Settings › «WiFi off on USB» (wifiAuto): WiFi stops once the app has been
    // on the USB link for OFF_AFTER_MS, and comes back ON_AFTER_MS after it is
    // gone, so the board is never out of reach (docs/design/usb-link.md).
    // `alive` is UsbLink::alive, asked on every pass.
    void setLinkAlive(bool (*alive)()) { _linkAlive = alive; }
    bool off() const { return _off; }
    // A boot join, scan or AP retry in progress; neither STA nor AP is settled.
    bool connecting() const { return _phase != Phase::Idle; }
    bool sleeping() const { return _sleepOn; }
    String ip() const;
    // AP name is derived from the MAC; set in begin(), never changed after.
    const String& apSsid() const { return _apSsid; }
    const String& mdnsHost() const { return _mdns; }   // e.g. "megaopen.local"

    // Second network (the head unit's hotspot), NVS namespace "net2".
    // Applied at the next boot or AP retry. Called on httpd: it persists and
    // stages; loop() (net_svc) takes the staged pair into the Strings it uses.
    // An empty pass keeps the stored one for the same ssid.
    void setNet2(const String& ssid, String pass);
    // Copies, safe from any task: snapshots net_svc refreshes under a lock,
    // never the Strings it reassigns.
    String net2Ssid() const;
    String curSsid() const;

    // Bracket a scan: the station interface only exists between these.
    void scanBegin();
    void scanEnd();

private:
    static constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;  // boot: then the AP comes up
    static constexpr uint32_t RETRY_JOIN_MS = 8000;        // a retry from AP mode
    static constexpr uint32_t SCAN_TIMEOUT_MS = 6000;
    static constexpr uint32_t RETRY_MS = 60000;   // periodic STA retry while in AP
    // STA down this long (drove away from home): bring the AP back, so the board
    // is reachable in the car again; the AP retry rejoins whichever net is near.
    static constexpr uint32_t STA_LOST_TO_AP_MS = 30000;

    static constexpr uint32_t OFF_AFTER_MS = 10000;   // the app this long on USB: WiFi off
    static constexpr uint32_t ON_AFTER_MS  = 30000;   // the app this long gone: WiFi back

    enum class Phase : uint8_t { Idle, Scanning, Joining };
    void start(uint32_t nowMs);   // STA (or a scan of two) if there are networks, else AP
    // The policy above; true while WiFi is off and loop() has nothing to do.
    bool linkPolicy(uint32_t nowMs);
    void startJoin(uint32_t nowMs, uint32_t timeoutMs);
    void pickFromScan(int n);

    bool sleepRequired() const;   // BT on => modem sleep is mandatory
    void applySleep();
    void startAp();
    // Binds mDNS to the interface up now; idempotent, call on every transition.
    void startMdns();

    void useNet(uint8_t i);
    void snap(char* dst, const String& src);   // under _snapMux

    Cfg::Config* _cfg = nullptr;
    // net_svc's own (and setup()'s, before net_svc starts). The first network
    // and the AP password are boot copies of cfg: see begin().
    String   _ssid1, _pass1, _apPass;
    bool     _btOn = false;
    String   _ssid2, _pass2;
    String   _curSsid, _curPass;
    // What other tasks may read, and what httpd hands over.
    mutable portMUX_TYPE _snapMux = portMUX_INITIALIZER_UNLOCKED;
    char     _curSnap[33] = {0};
    char     _ssid2Snap[33] = {0};
    char     _stSsid2[33] = {0};
    char     _stPass2[65] = {0};
    volatile bool _stDirty = false;
    uint8_t  _retryIdx = 0;
    String   _apSsid;
    String   _mdns;
    bool     _sta = false;
    bool     _mdnsUp = false;
    bool     _sleepOn = false;
    uint32_t _bootMs = 0;
    uint32_t _lastRetry = 0;
    Phase    _phase = Phase::Idle;
    uint32_t _deadline = 0;
    uint32_t _staDownAt = 0;     // 0 = STA up (or not in STA)
    bool   (*_linkAlive)() = nullptr;
    bool     _off = false;
    uint32_t _linkSince = 0;     // the app on USB since; 0 = not
    uint32_t _linkGoneAt = 0;    // the app gone since; 0 = there
};
