#include "ImprovSerial.h"
#include "Improv.h"
#include "WifiManager.h"
#include "../Version.h"
#include "../utils/LogBus.h"
#include <Arduino.h>
#include <WiFi.h>
#include <string.h>

namespace {

constexpr uint32_t SCAN_MAX_MS = 10000;   // a scan that never completes ends empty
constexpr uint32_t WRITE_WAIT_MS = 100;   // a host that stopped reading: drop

class SerialPort : public Improv::Port {
public:
    void write(const uint8_t* d, size_t n) override
    {
#if ARDUINO_USB_CDC_ON_BOOT && !ARDUINO_USB_MODE
        // TinyUSB CDC write() spins with no timeout while the host holds DTR
        // and does not read (main.cpp serialSink): wait a little, then drop.
        const uint32_t t0 = millis();
        while (Serial.availableForWrite() < (int)n) {
            if (millis() - t0 > WRITE_WAIT_MS) return;
            delay(2);
        }
#endif
        Serial.write(d, n);
    }
};

class WifiBoard : public Improv::Board {
public:
    WifiManager* w = nullptr;
    uint32_t scanAt = 0;
    bool scanOwn = false;    // scanBegin() was called: scanEnd() owed

    bool connected() override
    {
        return w->isSta() && !w->connecting() && WiFi.status() == WL_CONNECTED;
    }
    void url(char* out, size_t cap) override
    {
        snprintf(out, cap, "http://%s", WiFi.localIP().toString().c_str());
    }
    const char* firmware() override { return "MegaOpen"; }
    const char* version() override { return MEGAOPEN_VERSION; }
    const char* chip() override
    {
#if CONFIG_IDF_TARGET_ESP32S3
        return "ESP32-S3";
#elif CONFIG_IDF_TARGET_ESP32C3
        return "ESP32-C3";
#else
        return "ESP32";
#endif
    }
    const char* deviceName() override { return w->apSsid().c_str(); }

    void provision(const char* ssid, const char* pass) override
    {
        w->provision(ssid, pass, millis());
    }
    Join join() override
    {
        switch (w->provResult()) {
        case WifiManager::Prov::Pending: return Join::Pending;
        case WifiManager::Prov::Joined:  return Join::Joined;
        case WifiManager::Prov::Failed:  return Join::Failed;
        default:                         return Join::None;
        }
    }

    // Async, so net_svc keeps running. Not while WifiManager is joining or
    // scanning itself: the radio is its.
    bool scanStart() override
    {
        if (w->connecting() || w->off()) {
            LOGW("improv", "scan refused: wifi %s", w->off() ? "off (USB link)" : "busy joining");
            return false;
        }
        w->scanBegin();
        scanOwn = true;
        // 300 ms a channel: arduino-esp32 2.0.17 times an async scan out at
        // max_ms_per_chan * 20, and at 120 that is 2.4 s, shorter than a scan
        // of 13 channels: scanComplete() said "failed" at 2419 ms on the bench.
        const int16_t r = WiFi.scanNetworks(true, false, false, 300);
        if (r == WIFI_SCAN_FAILED) {
            LOGW("improv", "scan did not start (mode %d)", (int)WiFi.getMode());
            finish();
            return false;
        }
        scanAt = millis();
        return true;
    }
    int scanDone(Improv::Net* out, int cap) override
    {
        const int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING && millis() - scanAt < SCAN_MAX_MS) return -1;
        if (n < 0) LOGW("improv", "scan ended with %d after %u ms (mode %d)", n,
                        (unsigned)(millis() - scanAt), (int)WiFi.getMode());
        int k = 0;
        for (int i = 0; i < n; ++i) {
            const String s = WiFi.SSID(i);
            if (s.isEmpty() || s.length() > 32) continue;   // hidden
            const int8_t rssi = (int8_t)WiFi.RSSI(i);
            // One row per name, the strongest.
            int j = 0;
            while (j < k && strcmp(out[j].ssid, s.c_str())) ++j;
            if (j < k) { if (rssi > out[j].rssi) out[j].rssi = rssi; continue; }
            if (k == cap) continue;
            strncpy(out[k].ssid, s.c_str(), sizeof(out[k].ssid) - 1);
            out[k].ssid[sizeof(out[k].ssid) - 1] = 0;
            out[k].rssi = rssi;
            out[k].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
            ++k;
        }
        // Strongest first.
        for (int a = 1; a < k; ++a)
            for (int b = a; b > 0 && out[b].rssi > out[b - 1].rssi; --b) {
                const Improv::Net t = out[b]; out[b] = out[b - 1]; out[b - 1] = t;
            }
        finish();
        return k;
    }
    void finish()
    {
        WiFi.scanDelete();
        if (scanOwn) { w->scanEnd(); scanOwn = false; }
    }
};

SerialPort  s_port;
WifiBoard   s_board;
Improv::Session* s_session = nullptr;
uint32_t    s_said = 0;   // packets already logged about

}  // namespace

namespace ImprovSerial {

void begin(WifiManager& wifi)
{
    s_board.w = &wifi;
    static Improv::Session session(s_board, s_port);
    s_session = &session;
}

void tick(uint32_t nowMs)
{
    if (!s_session) return;
    // A burst is a packet or two (< 120 bytes); bounded so a terminal pasting
    // text cannot hold net_svc.
    for (int i = 0; i < 256 && Serial.available() > 0; ++i) {
        const int c = Serial.read();
        if (c < 0) break;
        s_session->feed((uint8_t)c, nowMs);
    }
    s_session->tick(nowMs);
    if (s_session->packets() != s_said) {
        if (!s_said) LOGI("improv", "client on the serial port");
        s_said = s_session->packets();
    }
}

}  // namespace ImprovSerial
