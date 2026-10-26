#include "../utils/LogBus.h"
#include "WifiManager.h"
#include "../Version.h"
#include "../Features.h"     // FEATURE_BLE decides whether the radio is shared
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>

void WifiManager::begin(Cfg::Config& cfg)
{
    _cfg = &cfg;
    // Boot copies: httpd saves new values into cfg at any time (POST /api/wifi,
    // /api/settings) and they apply at the next boot, so after this line no
    // String in cfg is read here except mdnsName, which nothing writes.
    _ssid1 = cfg.s().ssid;
    _pass1 = cfg.s().pass;
    _apPass = cfg.s().apPass;
    _btOn = cfg.s().bleMode != "off";
    {
        char name[32];
        snprintf(name, sizeof(name), "%s-%04X", cfg.s().apName.c_str(),
                 (uint16_t)(ESP.getEfuseMac() & 0xFFFF));
        _apSsid = name;
    }
    WiFi.persistent(false);
    {
        Preferences p;
        p.begin("net2", true);
        _ssid2 = p.getString("ssid", "");
        _pass2 = p.getString("pass", "");
        p.end();
        snap(_ssid2Snap, _ssid2);
    }

    // WiFi buffers stay dynamic. Default is 32+32 dynamic RX/TX buffers from the
    // DMA heap: free dips ~80 kB -> ~22 kB during a burst, then recovers.
    // useStaticBuffers(true) costs 35 kB permanently and measured worse
    // (`[win] min=13292 dma=168`, the empty-DRAM floor).

    // Disconnect reason tells coexistence (200) from router (8) from bad key (15).
    // Serial only: this runs on the WiFi event task.
    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
        LogBus::printf("[wifi] STA disconnected reason=%u heap=%u\n",
                      (unsigned)info.wifi_sta_disconnected.reason, (unsigned)ESP.getFreeHeap());
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t) {
        LogBus::printf("[wifi] STA got IP heap=%u\n", (unsigned)ESP.getFreeHeap());
    }, ARDUINO_EVENT_WIFI_STA_GOT_IP);

    start(millis());
    _bootMs = millis();
}

void WifiManager::start(uint32_t nowMs)
{
    if (_ssid1.length() || _ssid2.length()) {
        // mode() re-initialises the interface, so setSleep() must come after it.
        WiFi.mode(WIFI_STA);
        // Before begin(): with BT enabled, modem sleep must be on or the driver aborts.
        applySleep();
        WiFi.setHostname(_cfg->s().mdnsName.c_str());
        WiFi.setAutoReconnect(true);
        const bool has1 = _ssid1.length() > 0;
        const bool has2 = _ssid2.length() > 0;
        if (has1 && has2) {
            // Two known: scan in the background, home first. loop() picks.
            WiFi.scanNetworks(true, false, false, 120);
            _phase = Phase::Scanning;
            _deadline = nowMs + SCAN_TIMEOUT_MS;
        } else {
            useNet(has1 ? 0 : 1);
            startJoin(nowMs, CONNECT_TIMEOUT_MS);
        }
        LogBus::printf("[wifi] %s: %s, AP if nothing joins in %u s\n", _bootMs ? "start" : "boot",
                      _phase == Phase::Scanning ? "scanning" : "joining",
                      (unsigned)(CONNECT_TIMEOUT_MS / 1000));
    } else {
        startAp();
    }
}

bool WifiManager::linkPolicy(uint32_t nowMs)
{
    const bool link = _linkAlive && _linkAlive();
    if (link) { _linkGoneAt = 0; if (!_linkSince) _linkSince = nowMs ? nowMs : 1; }
    else      { _linkSince = 0;  if (!_linkGoneAt) _linkGoneAt = nowMs ? nowMs : 1; }
    // A plain bool, read without the settings lock (only Strings need it).
    const bool want = _cfg->s().wifiAuto;
    if (!_off) {
        // Not mid-join, and never with someone on our own AP.
        if (want && link && (uint32_t)(nowMs - _linkSince) >= OFF_AFTER_MS && _phase == Phase::Idle &&
            (_sta || WiFi.softAPgetStationNum() == 0)) {
            const uint32_t before = ESP.getFreeHeap();
            if (_mdnsUp) { MDNS.end(); _mdnsUp = false; _mdns = String(); }
            // Else the core's own reconnect answers the disconnect this causes
            // (reason 8) with WiFi.begin(), and WiFi is back in 3 s. start()
            // turns it on again.
            WiFi.setAutoReconnect(false);
            WiFi.mode(WIFI_OFF);
            _off = true;
            _sta = false;
            _staDownAt = 0;
            LogBus::printf("[wifi] off: the app is on USB (heap %u -> %u)", (unsigned)before,
                           (unsigned)ESP.getFreeHeap());
            // Not on into the rest of loop(): with _sta false it is the AP's
            // retry, which would join again at once.
            return true;
        }
        return false;
    }
    if (!want || (!link && (uint32_t)(nowMs - _linkGoneAt) >= ON_AFTER_MS)) {
        _off = false;
        LogBus::printf("[wifi] on again: %s", want ? "no app on USB" : "«WiFi off on USB» was switched off");
        _lastRetry = nowMs;
        start(nowMs);
        return false;
    }
    return true;
}

void WifiManager::startJoin(uint32_t nowMs, uint32_t timeoutMs)
{
    // In AP mode begin() turns STA on as well (AP_STA) for the attempt only.
    WiFi.begin(_curSsid.c_str(), _curPass.c_str());
    _phase = Phase::Joining;
    _deadline = nowMs + timeoutMs;
}

// Home first when both are on the air.
void WifiManager::pickFromScan(int n)
{
    bool see1 = false, see2 = false;
    for (int i = 0; i < n; ++i) {
        const String s = WiFi.SSID(i);
        if (s == _ssid1) see1 = true;
        if (s == _ssid2) see2 = true;
    }
    WiFi.scanDelete();
    useNet(see1 || !see2 ? 0 : 1);
    LogBus::printf("[wifi] pick \"%s\" (home %s, second %s)\n", _curSsid.c_str(),
                  see1 ? "seen" : "absent", see2 ? "seen" : "absent");
}

// http://megaopen.local. Called on every interface transition: the responder
// binds to the interfaces that exist when it starts, so a registration made
// before the STA joined silently never resolves.
void WifiManager::startMdns()
{
    // STA only: in AP mode multicast sends fail continuously with ENOMEM, and AP
    // clients use the fixed AP address anyway.
    if (!_sta) {
        if (_mdnsUp) { MDNS.end(); _mdnsUp = false; _mdns = String(); }
        return;
    }
    if (_mdnsUp) {
        MDNS.end();
        _mdnsUp = false;
        _mdns = String();
    }
    // Switchable to save heap: an abort under WiFi buffer pressure had a
    // backtrace in `_mdns_scheduler_run`. Off means no name, nothing else.
    if (!_cfg->s().mdnsOn) return;
    const String host = _cfg->s().mdnsName.length() ? _cfg->s().mdnsName : String("megaopen");
    if (!MDNS.begin(host.c_str())) return;
    MDNS.addService("http", "tcp", 80);
    // For OTA tools.
    MDNS.addService("arduino", "tcp", 3232);
    // For the phone and tablet apps (the voice alerts): they find the board on
    // the head unit's hotspot by this, where a .local name does not resolve.
    MDNS.addService("megaopen", "tcp", 80);
    MDNS.addServiceTxt("megaopen", "tcp", "fw", MEGAOPEN_VERSION);
    _mdnsUp = true;
    _mdns = host + ".local";
}

// Modem sleep is mandatory with BT up: the IDF WiFi driver aborts ("Should enable
// WiFi modem sleep when both WiFi and Bluetooth are enabled") and boot-loops.
// With BT off, sleep makes the station answer ARP/TCP a DTIM beacon late, so off.
// Derived, never stored, so no NVS value can reach the aborting combination.
// Boot bleMode, not FEATURE_BLE: what matters is whether the stack started. Not
// the saved one either: a bleMode saved as off runs with BT up until the reboot.
bool WifiManager::sleepRequired() const
{
    return _btOn;
}

void WifiManager::applySleep()
{
    _sleepOn = sleepRequired();
    WiFi.setSleep(_sleepOn);
}

void WifiManager::startAp()
{
    // Plain AP: permanent AP_STA with NimBLE starting aborted in BLE init.
    // Scanning raises STA on demand (scanBegin/scanEnd).
    WiFi.mode(WIFI_AP);
    applySleep();
    WiFi.softAP(_apSsid.c_str(), _apPass.c_str());
    _sta = false;
}

void WifiManager::loop(uint32_t nowMs)
{
    // A second network staged by httpd (setNet2): taken here, on the only task
    // that reads _ssid2/_pass2 after setup().
    if (_stDirty) {
        char s[sizeof(_stSsid2)], pw[sizeof(_stPass2)];
        portENTER_CRITICAL(&_snapMux);
        memcpy(s, _stSsid2, sizeof(s));
        memcpy(pw, _stPass2, sizeof(pw));
        _stDirty = false;
        portEXIT_CRITICAL(&_snapMux);
        _ssid2 = s;
        _pass2 = pw;
    }

    if (linkPolicy(nowMs)) return;

    if (_phase == Phase::Scanning) {
        const int n = WiFi.scanComplete();   // -1 running, -2 failed, else a count
        if (n == -1 && (int32_t)(nowMs - _deadline) < 0) return;
        pickFromScan(n > 0 ? n : 0);
        startJoin(nowMs, CONNECT_TIMEOUT_MS);
        return;
    }

    if (_phase == Phase::Joining) {
        if (WiFi.status() == WL_CONNECTED) {
            _phase = Phase::Idle;
            _sta = true;
            _staDownAt = 0;
            LogBus::printf("[wifi] joined \"%s\" ip=%s\n", _curSsid.c_str(),
                          WiFi.localIP().toString().c_str());
            startMdns();
        } else if ((int32_t)(nowMs - _deadline) >= 0) {
            // Plain AP again; the failed attempt cycled the radio through STA.
            _phase = Phase::Idle;
            _lastRetry = nowMs;
            LogBus::printf("[wifi] \"%s\" did not join; AP\n", _curSsid.c_str());
            startAp();
            startMdns();
        }
        return;
    }

    if (_sta) {
        if (WiFi.status() != WL_CONNECTED) {
            // The responder died with the interface; force a re-register below.
            _mdnsUp = false;
            if (!_staDownAt) _staDownAt = nowMs ? nowMs : 1;
            if ((uint32_t)(nowMs - _staDownAt) > STA_LOST_TO_AP_MS) {
                LogBus::printf("[wifi] \"%s\" gone %u s; AP\n", _curSsid.c_str(),
                              (unsigned)(STA_LOST_TO_AP_MS / 1000));
                _staDownAt = 0;
                _lastRetry = nowMs;
                startAp();
                startMdns();
            }
            // setAutoReconnect(true) does the retrying until then.
            return;
        }
        _staDownAt = 0;
        if (!_mdnsUp) startMdns();   // back after a drop, maybe a new lease
        return;
    }

    // In AP mode, retry the saved networks occasionally: the car may have been out
    // of range when it booted, or the head unit's hotspot came up after us.
    // NOT while a phone is on our AP: the attempt hops the radio off the AP's
    // channel, and that is what kept phones from connecting.
    if ((_ssid1.length() || _ssid2.length()) &&
        (uint32_t)(nowMs - _lastRetry) > RETRY_MS) {
        _lastRetry = nowMs;
        if (WiFi.softAPgetStationNum() > 0) return;
        // No scan from AP mode: alternate between the known networks.
        if (_ssid1.length() && _ssid2.length()) useNet(_retryIdx++ & 1);
        else useNet(_ssid1.length() ? 0 : 1);
        startJoin(nowMs, RETRY_JOIN_MS);
    }
}

// Raise STA only long enough to scan. applySleep() after every mode() call:
// without it power save stays off with BLE live and the next pairing fails with
// reason=0x208 supervision timeout.
// Not during our own scan or join: those own the mode until loop() settles them.
void WifiManager::scanBegin()
{
    if (_off) return;      // off for the USB link: the scan just fails
    if (!_sta && _phase == Phase::Idle) { WiFi.mode(WIFI_AP_STA); applySleep(); }
}

void WifiManager::scanEnd()
{
    if (_off) return;
    if (!_sta && _phase == Phase::Idle) { WiFi.mode(WIFI_AP); applySleep(); }
}

String WifiManager::ip() const
{
    return _sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
}

void WifiManager::useNet(uint8_t i)
{
    if (i == 0) { _curSsid = _ssid1; _curPass = _pass1; }
    else        { _curSsid = _ssid2;        _curPass = _pass2; }
    snap(_curSnap, _curSsid);
}

void WifiManager::snap(char* dst, const String& src)
{
    char b[33];
    strncpy(b, src.c_str(), sizeof(b) - 1);
    b[sizeof(b) - 1] = 0;
    portENTER_CRITICAL(&_snapMux);
    memcpy(dst, b, sizeof(b));
    portEXIT_CRITICAL(&_snapMux);
}

String WifiManager::curSsid() const
{
    char b[sizeof(_curSnap)];
    portENTER_CRITICAL(&_snapMux);
    memcpy(b, _curSnap, sizeof(b));
    portEXIT_CRITICAL(&_snapMux);
    return String(b);
}

String WifiManager::net2Ssid() const
{
    char b[sizeof(_ssid2Snap)];
    portENTER_CRITICAL(&_snapMux);
    memcpy(b, _ssid2Snap, sizeof(b));
    portEXIT_CRITICAL(&_snapMux);
    return String(b);
}

// httpd: persist, and stage for loop() on net_svc. The Strings that task
// joins with are never written from here.
void WifiManager::setNet2(const String& ssid, String pass)
{
    Preferences p;
    p.begin("net2", false);
    // Empty keeps the stored password while the name stays; a new name with
    // no password is an open network.
    if (pass.isEmpty() && ssid == p.getString("ssid", "")) pass = p.getString("pass", "");
    p.putString("ssid", ssid);
    p.putString("pass", pass);
    p.end();
    char s[sizeof(_stSsid2)], pw[sizeof(_stPass2)];
    strncpy(s, ssid.c_str(), sizeof(s) - 1);  s[sizeof(s) - 1] = 0;
    strncpy(pw, pass.c_str(), sizeof(pw) - 1); pw[sizeof(pw) - 1] = 0;
    portENTER_CRITICAL(&_snapMux);
    memcpy(_stSsid2, s, sizeof(s));
    memcpy(_stPass2, pw, sizeof(pw));
    memcpy(_ssid2Snap, s, sizeof(s));
    _stDirty = true;
    portEXIT_CRITICAL(&_snapMux);
}
