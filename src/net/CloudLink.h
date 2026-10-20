#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "freertos/FreeRTOS.h"

// Link to the cloud/ Cloudflare Worker:
//   * logs: text lines kept in a ring, batched up every INGEST_MS with state;
//     offline, the ring keeps the newest lines and counts losses.
//   * deploy: every MANIFEST_MS, install a newer build into the other OTA slot
//     — ONLY while the car is idle (setIdle(): main.cpp says no key and no
//     head-unit frame for IDLE_MS). Erasing the slot stalls both cores sector
//     by sector, and the reboot drops the panel and the keys: neither may
//     happen while someone is driving. An installed build waits; the reboot
//     into it happens when the car is idle again, or at the next power-up.
//   * rollback: APP_ROLLBACK is on, so an OTA image boots PENDING_VERIFY and is
//     marked valid once it has been healthy for CONFIRM_MS without a break (no
//     crash, WiFi up at least once so the next OTA can arrive, heap above
//     HEALTH_HEAP_MIN, and every critical task in the registry running —
//     main.cpp builds that). TRIAL_MS without that reverts, and the version is
//     never fetched again. A trial cut short by a reset reverts too (the
//     bootloader's rule), but the version is blacklisted only when that reset
//     was the image's fault (panic or watchdog); a power cut or brownout —
//     ignition off, cranking — only lets it be fetched again. The display is
//     deliberately not a condition: a bench without the panel must not roll
//     good builds back.
//
// TLS wants ~40 kB on a board idling at ~80 kB (dips to ~22 kB), so a request
// starts only with FREE_MIN free and a LARGEST_MIN block, one at a time, on
// this module's task; skipped cycles are counted.
class CloudLink {
public:
    // Fills the "state" object of a batch. Called on the cloud task.
    using StateFn = void (*)(JsonObject out);

    // The module's own log sink (main's logf).
    static void setLog(void (*log)(const char*));
    // Called on the cloud task after every pass, with how long it took: how
    // main.cpp puts this task in the task registry without net/ depending on
    // app/. The task exists only once the cloud is switched on.
    using WakeHook = void (*)(uint32_t busyUs);
    static void onWake(WakeHook h);
    void begin(StateFn state);
    // Every log line. Any task; a short critical section, never blocks.
    // Once begin() has run with the cloud off, it returns at once: nothing
    // would ever drain the ring.
    void tap(const char* s, int n);

    // "The car is not in use", asked before an install and before the reboot
    // into it. Called on the cloud task. Unset = never idle: nothing installs.
    using IdleFn = bool (*)(uint32_t nowMs);
    static void setIdle(IdleFn f);
    // From loop(): drives the rollback verdict for a freshly installed image.
    // `healthy`: the caller's basic health check, evaluated every call.
    void loop(uint32_t nowMs, bool healthy);
    static constexpr uint32_t HEALTH_HEAP_MIN = 20000;   // bytes free for "healthy"

    void status(JsonObject out) const;
    // {url, token, on} — persisted. Returns false on a malformed request.
    bool configure(JsonVariantConst in);

    // Deployed version string if from the cloud, else MEGAOPEN_VERSION.
    const char* fw() const { return _fw; }

private:
    static constexpr uint32_t FIRST_MS     = 15000;
    static constexpr uint32_t INGEST_MS    = 60000;
    static constexpr uint32_t MANIFEST_MS  = 300000;
    static constexpr uint32_t TRIAL_MS     = 180000;
    static constexpr uint32_t CONFIRM_MS   = 60000;
    static constexpr uint32_t FREE_MIN     = 65000;
    static constexpr uint32_t LARGEST_MIN  = 48000;
    static constexpr int      RING         = 4096;
    static constexpr int      SNAP_MAX     = 3584;
    static constexpr int      BODY_MAX     = 8192;

    static void taskThunk(void* self);
    void task();
    bool heapOk();
    bool ingest();
    void checkManifest();
    void uploadCoredump();
    bool install(const char* version, const char* path);
    int  request(const char* method, const char* path, const char* body, int bodyLen,
                 char* resp, int respMax);

    // ---- persisted (NVS namespace "cloud") ----------------------------------
    // Written by configure() on httpd, read by the cloud task: char arrays
    // copied in and out under _cfgMux, never a String one task reallocates
    // while the other copies it.
    static constexpr size_t URL_MAX = 160, TOKEN_MAX = 160;
    char     _url[URL_MAX] = {0};     // https://megaopen.<acct>.workers.dev (no trailing /)
    char     _token[TOKEN_MAX] = {0};
    mutable portMUX_TYPE _cfgMux = portMUX_INITIALIZER_UNLOCKED;
    void     creds(char* url, char* token) const;   // URL_MAX / TOKEN_MAX buffers
    bool     configured() const;                     // on, with a URL
    volatile bool _on = false;
    bool     _begun = false;
    char     _fw[32]  = {0};    // what we report
    char     _pend[32] = {0};   // installed, not yet proven
    char     _rej[32]  = {0};   // proven bad — never fetched again
    uint32_t _boot = 0;

    // ---- the log ring (tap side) ---------------------------------------------
    char         _ring[RING];
    int          _len = 0;
    uint32_t     _trimmed = 0;       // bytes ever dropped off the front
    uint32_t     _lostLines = 0;
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

    // ---- trial / rollback ----------------------------------------------------
    bool     _trial = false;       // running image is PENDING_VERIFY
    bool     _verdict = false;
    uint32_t _healthySince = 0;    // 0 = not healthy at the last sample
    // Installed into the other slot, reboot owed: no more manifests, and the
    // restart waits for the car to be idle (or the next power-up boots it).
    bool     _installed = false;

    // ---- counters for /api/cloud ----------------------------------------------
    StateFn  _state = nullptr;
    TaskHandle_t _taskH = nullptr;
    uint32_t _ok = 0, _fail = 0, _skipHeap = 0, _lastOkMs = 0, _otaTries = 0;
    int      _lastHttp = 0;
    char     _lastErr[48] = {0};
    char     _dev[16] = {0};
    bool     _coreDone = false;
};
