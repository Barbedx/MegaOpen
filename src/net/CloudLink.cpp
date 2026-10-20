#include "CloudLink.h"
#include "../Version.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_ota_ops.h>
// The IDF certificate bundle. Declared, not included: <esp_crt_bundle.h>
// resolves to WiFiClientSecure's copy, which names a different function.
extern "C" esp_err_t esp_crt_bundle_attach(void* conf);
#include <esp_core_dump.h>
#include <esp_partition.h>
#include <esp_heap_caps.h>
#include <string.h>

// Stops the core marking every image valid at boot (esp32-hal-misc.c); the
// verdict is CloudLink::loop()'s. USB-flashed images are never PENDING_VERIFY.
extern "C" bool verifyRollbackLater() { return true; }

static void (*s_log)(const char*) = nullptr;
static CloudLink::IdleFn s_idle = nullptr;
void CloudLink::setIdle(IdleFn f) { s_idle = f; }
static bool carIdle() { return s_idle && s_idle(millis()); }

// Into the LogBus (main's setLog), which reaches Serial, the web and the flash
// log from its own task. Never Serial directly: on a TinyUSB CDC build a write
// spins while the host holds DTR without reading, and this runs on net_svc.
static void note(const char* fmt, ...)
{
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (s_log) s_log(b);
}

void CloudLink::creds(char* url, char* token) const
{
    portENTER_CRITICAL(&_cfgMux);
    memcpy(url, _url, URL_MAX);
    memcpy(token, _token, TOKEN_MAX);
    portEXIT_CRITICAL(&_cfgMux);
}

bool CloudLink::configured() const
{
    return _on && _url[0];   // one byte: no lock needed for a yes/no
}

// Is the reset that ended a trial the image's fault?
static bool resetIsImageFault(esp_reset_reason_t r)
{
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
           r == ESP_RST_WDT;
}

static const char* resetName(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "ext";
        case ESP_RST_SW:        return "sw";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

static void copyStr(char* dst, size_t n, const String& s)
{
    strncpy(dst, s.c_str(), n - 1);
    dst[n - 1] = 0;
}

void CloudLink::setLog(void (*log)(const char*)) { s_log = log; }

static CloudLink::WakeHook s_wake = nullptr;
void CloudLink::onWake(WakeHook h) { s_wake = h; }

void CloudLink::begin(StateFn state)
{
    _state = state;

    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(_dev, sizeof(_dev), "mo-%02x%02x%02x", mac[3], mac[4], mac[5]);

    Preferences p;
    p.begin("cloud", false);
    copyStr(_url,   URL_MAX,   p.getString("url", ""));
    copyStr(_token, TOKEN_MAX, p.getString("tok", ""));
    _on    = p.getBool("on", false);
    char cur[32];
    copyStr(cur,   sizeof(cur),   p.getString("cur", MEGAOPEN_VERSION));
    copyStr(_pend, sizeof(_pend), p.getString("pend", ""));
    copyStr(_rej,  sizeof(_rej),  p.getString("rej", ""));
    _boot = p.getUInt("boot", 0) + 1;
    p.putUInt("boot", _boot);

    const esp_partition_t* run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    _trial = run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
             st == ESP_OTA_IMG_PENDING_VERIFY;

    if (_trial) {
        // On probation: report the version we were installed as.
        copyStr(_fw, sizeof(_fw), String(_pend[0] ? _pend : "local-ota"));
        note("[cloud] build %s on trial: confirmed after %us healthy, rolled back after %us",
             _fw, (unsigned)(CONFIRM_MS / 1000), (unsigned)(TRIAL_MS / 1000));
    } else {
        if (_pend[0]) {
            // Pending but not running: the bootloader rolled back, because the
            // trial boot was reset before its verdict. This boot's reset
            // reason is that reset. Only a crash or a watchdog is the image's
            // fault; ignition off, a cranking brownout or a user reboot is not,
            // and the build may be fetched again.
            const esp_reset_reason_t why = esp_reset_reason();
            if (resetIsImageFault(why)) {
                note("[cloud] build %s failed its trial (reset=%s, rolled back) - blacklisted",
                     _pend, resetName(why));
                copyStr(_rej, sizeof(_rej), String(_pend));
                p.putString("rej", _rej);
            } else {
                note("[cloud] build %s: trial cut short by reset=%s, rolled back; "
                     "not blacklisted, it may be installed again", _pend, resetName(why));
            }
            _pend[0] = 0;
            p.putString("pend", "");
        }
        copyStr(_fw, sizeof(_fw), String(cur));
    }
    p.end();

    note("[cloud] dev=%s fw=%s boot=%u reset=%s %s", _dev, _fw, (unsigned)_boot,
         resetName(esp_reset_reason()),
         configured() ? _url : "(off)");

    _begun = true;
    if (configured() && !_taskH)
        xTaskCreatePinnedToCore(taskThunk, "cloud", 7168, this, 1, &_taskH, 0);
}

// ---- the ring ----------------------------------------------------------------
// Linear, not circular: readers take a prefix, overflow drops the oldest.
void CloudLink::tap(const char* s, int n)
{
    if (n <= 0) return;
    // Before begin() the boot lines are kept (the cloud may turn out to be
    // on); after it, a cloud that is off never drains the ring, and a full
    // ring would memmove ~3 kB under the spinlock on every line.
    if (_begun && !configured()) return;
    if (n > 240) n = 240;
    portENTER_CRITICAL(&_mux);
    if (_len + n + 1 > RING) {
        // Drop whole lines from the front, at least a quarter of the ring.
        int cut = RING / 4;
        if (cut < n + 1) cut = n + 1;
        while (cut < _len && _ring[cut - 1] != '\n') ++cut;
        if (cut > _len) cut = _len;
        for (int i = 0; i < cut; ++i) if (_ring[i] == '\n') ++_lostLines;
        memmove(_ring, _ring + cut, _len - cut);
        _len -= cut;
        _trimmed += cut;
    }
    memcpy(_ring + _len, s, n);
    _len += n;
    _ring[_len++] = '\n';
    portEXIT_CRITICAL(&_mux);
}

// ---- trial verdict -------------------------------------------------------------
void CloudLink::loop(uint32_t nowMs, bool healthy)
{
    if (!_trial || _verdict) return;
    // Healthy for CONFIRM_MS without a break, not one good sample after it.
    if (!healthy)              _healthySince = 0;
    else if (!_healthySince)   _healthySince = nowMs ? nowMs : 1;
    if (_healthySince && (uint32_t)(nowMs - _healthySince) >= CONFIRM_MS) {
        _verdict = true;
        esp_ota_mark_app_valid_cancel_rollback();
        Preferences p;
        p.begin("cloud", false);
        if (_pend[0]) p.putString("cur", _pend);
        p.putString("pend", "");
        p.end();
        _pend[0] = 0;
        _trial = false;
        note("[cloud] build %s CONFIRMED (healthy %us)", _fw, (unsigned)(CONFIRM_MS / 1000));
        return;
    }
    if (nowMs > TRIAL_MS) {
        _verdict = true;
        note("[cloud] build %s never became healthy - rolling back", _fw);
        Preferences p;
        p.begin("cloud", false);
        p.putString("rej", _fw);
        p.putString("pend", "");
        p.end();
        delay(300);
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

// ---- config ----------------------------------------------------------------------
void CloudLink::status(JsonObject o) const
{
    char url[URL_MAX], tok[TOKEN_MAX];
    creds(url, tok);
    o["on"]       = (bool)_on;
    o["url"]      = url;
    o["tokenSet"] = tok[0] != 0;
    o["installed"] = _installed;   // a build waits in the other slot for an idle car
    o["dev"]      = _dev;
    o["fw"]       = _fw;
    o["boot"]     = _boot;
    o["trial"]    = _trial;
    o["pend"]     = _pend;
    o["rejected"] = _rej;
    o["ok"]       = _ok;
    o["fail"]     = _fail;
    o["skipHeap"] = _skipHeap;
    o["lastHttp"] = _lastHttp;
    o["lastErr"]  = _lastErr;
    o["lastOkAgoMs"] = _lastOkMs ? (uint32_t)(millis() - _lastOkMs) : 0;
    o["otaTries"] = _otaTries;
    o["ringBytes"] = _len;
    o["lostLines"] = _lostLines;
    o["task"]     = _taskH != nullptr;
    if (_taskH) o["stackFree"] = uxTaskGetStackHighWaterMark(_taskH);
}

bool CloudLink::configure(JsonVariantConst in)
{
    if (!in.is<JsonObjectConst>()) return false;
    // Checked before anything is stored: too long is a malformed request.
    if (in["url"].is<const char*>() && strlen(in["url"].as<const char*>()) >= URL_MAX) return false;
    if (in["token"].is<const char*>() && strlen(in["token"].as<const char*>()) >= TOKEN_MAX) return false;
    Preferences p;
    p.begin("cloud", false);
    if (in["url"].is<const char*>()) {
        char u[URL_MAX];
        strncpy(u, in["url"].as<const char*>(), URL_MAX - 1);
        u[URL_MAX - 1] = 0;
        size_t n = strlen(u);
        while (n && u[n - 1] == '/') u[--n] = 0;
        p.putString("url", u);
        portENTER_CRITICAL(&_cfgMux);
        memcpy(_url, u, URL_MAX);
        portEXIT_CRITICAL(&_cfgMux);
    }
    if (in["token"].is<const char*>()) {
        char t[TOKEN_MAX];
        strncpy(t, in["token"].as<const char*>(), TOKEN_MAX - 1);
        t[TOKEN_MAX - 1] = 0;
        p.putString("tok", t);
        portENTER_CRITICAL(&_cfgMux);
        memcpy(_token, t, TOKEN_MAX);
        portEXIT_CRITICAL(&_cfgMux);
    }
    if (in["on"].is<bool>()) {
        _on = in["on"].as<bool>();
        p.putBool("on", _on);
    }
    if (in["clearRejected"] | false) {
        _rej[0] = 0;
        p.putString("rej", "");
    }
    p.end();
    if (configured() && !_taskH)
        xTaskCreatePinnedToCore(taskThunk, "cloud", 7168, this, 1, &_taskH, 0);
    return true;
}

// ---- the task ---------------------------------------------------------------------
void CloudLink::taskThunk(void* self) { static_cast<CloudLink*>(self)->task(); }

void CloudLink::task()
{
    vTaskDelay(pdMS_TO_TICKS(FIRST_MS));
    uint32_t lastIngest = 0, lastManifest = 0;
    bool first = true;
    for (;;) {
        const uint32_t now = millis();
        const uint32_t t0 = micros();
        if (_installed) {
            // A build is waiting in the other slot. Reboot into it only when
            // the car is idle; otherwise the next power-up boots it.
            if (carIdle()) {
                note("[cloud] car idle - rebooting into the installed build");
                delay(300);
                esp_restart();   // the shutdown handler flushes the log
            }
        } else if (configured() && WiFi.status() == WL_CONNECTED) {
            if (first || now - lastIngest >= INGEST_MS) {
                lastIngest = now;
                if (ingest()) {
                    first = false;
                    if (!_coreDone) uploadCoredump();
                }
            }
            // No deploys on probation: keep the rollback target intact.
            if (!first && !_trial && (lastManifest == 0 || now - lastManifest >= MANIFEST_MS)) {
                lastManifest = now;
                checkManifest();
            }
        }
        if (s_wake) s_wake(micros() - t0);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

bool CloudLink::heapOk()
{
    const uint32_t freeB = ESP.getFreeHeap();
    const uint32_t big   = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (freeB >= FREE_MIN && big >= LARGEST_MIN) return true;
    ++_skipHeap;
    snprintf(_lastErr, sizeof(_lastErr), "heap free=%u big=%u", (unsigned)freeB, (unsigned)big);
    return false;
}

// One HTTPS exchange. Returns the status code, or -1 on a transport error.
int CloudLink::request(const char* method, const char* path, const char* body, int bodyLen,
                       char* resp, int respMax)
{
    char base[URL_MAX], tok[TOKEN_MAX];
    creds(base, tok);
    String url = String(base) + path;
    esp_http_client_config_t c = {};
    c.url               = url.c_str();
    c.method            = strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    c.timeout_ms        = 10000;
    c.buffer_size       = 1024;
    c.buffer_size_tx    = 1024;
    c.crt_bundle_attach = esp_crt_bundle_attach;
    c.keep_alive_enable = false;
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) { snprintf(_lastErr, sizeof(_lastErr), "http init"); return -1; }
    String auth = String("Bearer ") + tok;
    esp_http_client_set_header(h, "Authorization", auth.c_str());
    if (body) esp_http_client_set_header(h, "Content-Type", "application/json");

    int status = -1;
    esp_err_t e = esp_http_client_open(h, body ? bodyLen : 0);
    if (e == ESP_OK) {
        if (body && esp_http_client_write(h, body, bodyLen) != bodyLen) {
            snprintf(_lastErr, sizeof(_lastErr), "write short");
        } else if (esp_http_client_fetch_headers(h) >= 0) {
            status = esp_http_client_get_status_code(h);
            int got = 0;
            if (resp && respMax > 1) {
                int r;
                while (got < respMax - 1 &&
                       (r = esp_http_client_read(h, resp + got, respMax - 1 - got)) > 0)
                    got += r;
                resp[got] = 0;
            }
        } else {
            snprintf(_lastErr, sizeof(_lastErr), "no headers");
        }
    } else {
        snprintf(_lastErr, sizeof(_lastErr), "open %s", esp_err_to_name(e));
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    _lastHttp = status;
    return status;
}

// Appends `s[0..n)` to out as JSON string content. Returns bytes written, or -1
// if it would not fit.
static int jsonEscape(char* out, int room, const char* s, int n)
{
    int w = 0;
    for (int i = 0; i < n; ++i) {
        const unsigned char ch = (unsigned char)s[i];
        char esc[7];
        int k;
        if (ch == '"' || ch == '\\') { esc[0] = '\\'; esc[1] = ch; k = 2; }
        else if (ch < 0x20) { k = snprintf(esc, sizeof(esc), "\\u%04x", ch); }
        else { esc[0] = ch; k = 1; }
        if (w + k > room) return -1;
        memcpy(out + w, esc, k);
        w += k;
    }
    return w;
}

bool CloudLink::ingest()
{
    if (!heapOk()) return false;

    char* body = (char*)malloc(BODY_MAX);
    if (!body) { ++_skipHeap; return false; }

    // Envelope via ArduinoJson; lines escaped into the tail of the same buffer.
    int n;
    {
        JsonDocument d;
        d["dev"]   = _dev;
        d["fw"]    = _fw;
        d["boot"]  = _boot;
        d["up"]    = millis();
        d["reset"] = resetName(esp_reset_reason());
        d["heap"]  = ESP.getFreeHeap();
        d["minHeap"] = ESP.getMinFreeHeap();
        d["rssi"]  = WiFi.RSSI();
        d["lost"]  = _lostLines;
        if (_state) _state(d["state"].to<JsonObject>());
        n = serializeJson(d, body, BODY_MAX - 64);
    }
    if (n <= 1 || body[n - 1] != '}') { free(body); snprintf(_lastErr, sizeof(_lastErr), "envelope"); return false; }
    --n;                                   // reopen the object
    n += snprintf(body + n, BODY_MAX - n, ",\"lines\":[");

    // Whole-line prefix of the ring, copied under the lock, escaped outside it.
    // `consumed` = ring bytes sent; dropped after, less whatever tap() trimmed.
    char* snap = (char*)malloc(SNAP_MAX);
    if (!snap) { free(body); ++_skipHeap; return false; }
    int snapLen;
    uint32_t trimmedAt;
    portENTER_CRITICAL(&_mux);
    snapLen = _len < SNAP_MAX ? _len : SNAP_MAX;
    while (snapLen > 0 && _ring[snapLen - 1] != '\n') --snapLen;
    memcpy(snap, _ring, snapLen);
    trimmedAt = _trimmed;
    portEXIT_CRITICAL(&_mux);

    int consumed = 0;
    bool firstLine = true;
    for (int i = 0; i < snapLen;) {
        int e = i;
        while (e < snapLen && snap[e] != '\n') ++e;
        const int room = BODY_MAX - n - 8;
        const int mark = n;
        if (!firstLine) body[n++] = ',';
        body[n++] = '"';
        const int w = jsonEscape(body + n, room - 3, snap + i, e - i);
        if (w < 0) { n = mark; break; }
        n += w;
        body[n++] = '"';
        firstLine = false;
        i = e + 1;
        consumed = i;
    }
    free(snap);
    n += snprintf(body + n, BODY_MAX - n, "]}");

    const int st = request("POST", "/v1/ingest", body, n, nullptr, 0);
    free(body);
    if (st == 200) {
        ++_ok;
        _lastOkMs = millis();
        _lastErr[0] = 0;
        portENTER_CRITICAL(&_mux);
        int drop = consumed - (int)(_trimmed - trimmedAt);
        if (drop > _len) drop = _len;
        if (drop > 0) { memmove(_ring, _ring + drop, _len - drop); _len -= drop; _trimmed += drop; }
        portEXIT_CRITICAL(&_mux);
        return true;
    }
    ++_fail;
    if (st > 0) snprintf(_lastErr, sizeof(_lastErr), "ingest http %d", st);
    return false;
}

void CloudLink::checkManifest()
{
    if (!heapOk()) return;
    char path[96];
    snprintf(path, sizeof(path), "/v1/manifest?dev=%s&fw=%s", _dev, _fw);
    char resp[256];
    const int st = request("GET", path, nullptr, 0, resp, sizeof(resp));
    if (st == 204) return;
    if (st != 200) { ++_fail; snprintf(_lastErr, sizeof(_lastErr), "manifest http %d", st); return; }

    JsonDocument d;
    if (deserializeJson(d, resp)) { snprintf(_lastErr, sizeof(_lastErr), "manifest json"); return; }
    const char* ver = d["version"] | "";
    const char* fwp = d["path"] | "";
    if (!ver[0] || !fwp[0]) return;
    if (strcmp(ver, _fw) == 0) return;
    if (strcmp(ver, _rej) == 0) return;   // proven bad on this board
    // Never while the car is in use: the erase stalls both cores and the
    // reboot drops the panel and the keys. Asked again next manifest.
    if (!carIdle()) {
        snprintf(_lastErr, sizeof(_lastErr), "%.24s waits for an idle car", ver);
        return;
    }
    install(ver, fwp);
}

static char s_auth[160];
static esp_err_t otaInit(esp_http_client_handle_t h)
{
    return esp_http_client_set_header(h, "Authorization", s_auth);
}

bool CloudLink::install(const char* version, const char* path)
{
    ++_otaTries;
    note("[cloud] installing %s (running %s)", version, _fw);
    char base[URL_MAX], tok[TOKEN_MAX];
    creds(base, tok);
    String url = String(base) + path;
    snprintf(s_auth, sizeof(s_auth), "Bearer %s", tok);

    esp_http_client_config_t c = {};
    c.url               = url.c_str();
    c.timeout_ms        = 20000;
    c.buffer_size       = 2048;
    c.buffer_size_tx    = 1024;
    c.crt_bundle_attach = esp_crt_bundle_attach;
    c.keep_alive_enable = false;
    esp_https_ota_config_t oc = {};
    oc.http_config         = &c;
    oc.http_client_init_cb = otaInit;

    // Recorded before the reboot: the next boot's trial identity.
    Preferences p;
    p.begin("cloud", false);
    p.putString("pend", version);
    p.end();

    const uint32_t t0 = millis();
    esp_https_ota_handle_t oh = nullptr;
    esp_err_t e = esp_https_ota_begin(&oc, &oh);
    if (e == ESP_OK) {
        while ((e = esp_https_ota_perform(oh)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {}
        if (e == ESP_OK && !esp_https_ota_is_complete_data_received(oh)) e = ESP_FAIL;
        const esp_err_t f = e == ESP_OK ? esp_https_ota_finish(oh) : (esp_https_ota_abort(oh), e);
        e = f;
    }
    if (e != ESP_OK) {
        p.begin("cloud", false);
        p.putString("pend", "");
        p.end();
        ++_fail;
        snprintf(_lastErr, sizeof(_lastErr), "ota %s", esp_err_to_name(e));
        note("[cloud] install %s FAILED: %s", version, esp_err_to_name(e));
        return false;
    }
    _installed = true;
    note("[cloud] installed %s in %us - reboots into trial once the car is idle, "
         "or at the next power-up", version, (unsigned)((millis() - t0) / 1000));
    // Still idle after the download (a minute or so): reboot now.
    if (carIdle()) {
        delay(500);
        esp_restart();   // the shutdown handler flushes the log
    }
    return true;
}

// A crash leaves an ELF core dump in its partition. Ship it once, then erase, so
// the next crash has somewhere to go.
void CloudLink::uploadCoredump()
{
    _coreDone = true;
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) return;
    if (size > 64 * 1024) {
        note("[cloud] coredump %u bytes - too big to ship, erased", (unsigned)size);
        esp_core_dump_image_erase();
        return;
    }
    if (!heapOk()) { _coreDone = false; return; }

    char base[URL_MAX], tok[TOKEN_MAX];
    creds(base, tok);
    char path[URL_MAX + 96];
    snprintf(path, sizeof(path), "%s/v1/coredump?dev=%s&fw=%s", base, _dev, _fw);
    esp_http_client_config_t c = {};
    c.url               = path;
    c.method            = HTTP_METHOD_POST;
    c.timeout_ms        = 15000;
    c.buffer_size       = 1024;
    c.buffer_size_tx    = 1024;
    c.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) { _coreDone = false; return; }
    String auth = String("Bearer ") + tok;
    esp_http_client_set_header(h, "Authorization", auth.c_str());
    esp_http_client_set_header(h, "Content-Type", "application/octet-stream");

    bool ok = false;
    if (esp_http_client_open(h, size) == ESP_OK) {
        uint8_t buf[512];
        size_t off = 0;
        ok = true;
        while (off < size) {
            const size_t k = size - off < sizeof(buf) ? size - off : sizeof(buf);
            if (esp_flash_read(nullptr, buf, addr + off, k) != ESP_OK ||
                esp_http_client_write(h, (const char*)buf, k) != (int)k) { ok = false; break; }
            off += k;
        }
        if (ok) ok = esp_http_client_fetch_headers(h) >= 0 && esp_http_client_get_status_code(h) == 200;
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    if (ok) {
        note("[cloud] coredump %u bytes shipped", (unsigned)size);
        esp_core_dump_image_erase();
    } else {
        _coreDone = false;   // try again next cycle
    }
}
