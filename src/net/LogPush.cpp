#include "LogPush.h"
#if FEATURE_LOG_PUSH
#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <mbedtls/base64.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "GhRoots.h"
#include "TlsJob.h"
#include "../Version.h"
#include "../app/TaskPlan.h"
#include "../utils/LogBus.h"

namespace LogPush
{
    namespace
    {
        constexpr int      TIMEOUT_MS = 20000;
        constexpr uint32_t FIRST_MS   = 20000;   // after the link comes up: let the boot settle
        constexpr uint32_t DRAIN_MS   = 3000;    // a backlog waits: the next file soon
        // What counts as a backlog: a spool that filled offline (the garage).
        // The lines logged during the push itself (a few kB in its seconds) wait
        // for the next minute; draining on those sent a file every ~8 s.
        constexpr uint32_t BACKLOG_BYTES = 16 * 1024;
        constexpr uint32_t CAP_PS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        constexpr uint32_t CAP_IN = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        void* psMalloc(size_t n) { return heap_caps_malloc_prefer(n, 2, CAP_PS, CAP_IN); }
        uint32_t heapNow() { return heap_caps_get_free_size(CAP_IN); }

        // ---- the spool: a byte ring of whole lines, in absolute offsets ----
        // [s_o, s_w) is held; [s_p, s_w) is not pushed yet. Only the LogBus task
        // writes (sink), only the job reads; s_lock is held for a memcpy each.
        SemaphoreHandle_t s_lock = nullptr;
        uint8_t* s_buf  = nullptr;
        size_t   s_size = 0;
        uint64_t s_w = 0, s_o = 0, s_p = 0;

        void ringPut(uint64_t at, const void* src, size_t n)
        {
            const size_t i = (size_t)(at % s_size), k = n < s_size - i ? n : s_size - i;
            memcpy(s_buf + i, src, k);
            if (k < n) memcpy(s_buf, (const uint8_t*)src + k, n - k);
        }
        void ringGet(uint64_t at, void* dst, size_t n)
        {
            const size_t i = (size_t)(at % s_size), k = n < s_size - i ? n : s_size - i;
            memcpy(dst, s_buf + i, k);
            if (k < n) memcpy((uint8_t*)dst + k, s_buf, n - k);
        }

        // ---- settings and state, under s_mux ----
        portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
        bool     s_on = false;
        char     s_tok[TOKEN_MAX + 1] = "";
        char     s_repo[REPO_MAX + 1] = "";
        char     s_dev[16] = "";
        State    s_state = State::Off;
        char     s_err[48] = "";
        int      s_status = 0;
        uint32_t s_files = 0, s_sent = 0, s_lost = 0, s_heapMin = 0;
        uint32_t s_okAt = 0;            // millis() of the last good push, 0 none
        uint32_t s_nextAt = 0;
        uint32_t s_backoff = 0;         // 0: none; else the wait after a failed try
        bool     s_nextSet = false;     // s_nextAt holds a plan (not before the first online tick)
        bool     s_busy = false;        // a job task exists
        uint32_t s_part = 0;            // the next file's number this boot
        uint32_t s_boot = 0;            // sys/boots, read by the first job

        bool ready() { return s_buf && s_on && s_tok[0] && s_repo[0]; }

        void setErr(State st, const char* code, int status)
        {
            portENTER_CRITICAL(&s_mux);
            s_state = st;
            snprintf(s_err, sizeof(s_err), "%s", code);
            s_status = status;
            portEXIT_CRITICAL(&s_mux);
        }

        // owner/name, ASCII letters, digits, '-', '_', '.'; exactly one '/'.
        bool repoOk(const char* r)
        {
            const size_t n = strlen(r);
            if (n < 3 || n > REPO_MAX) return false;
            int slash = 0;
            for (size_t i = 0; i < n; ++i) {
                const char c = r[i];
                if (c == '/') { if (++slash > 1 || i == 0 || i == n - 1) return false; continue; }
                if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') return false;
            }
            return slash == 1;
        }
        bool tokenOk(const char* t)
        {
            const size_t n = strlen(t);
            if (n < 20 || n > TOKEN_MAX) return false;
            for (size_t i = 0; i < n; ++i)
                if (!isalnum((unsigned char)t[i]) && t[i] != '_') return false;
            return true;
        }

        // What the next file carries: [*from, *from + n) of the spool into out
        // (CHUNK_MAX), whole lines only. *lost: bytes the ring dropped before
        // they were pushed (the cut line at the front is skipped too).
        size_t takeChunk(char* out, uint64_t& from, uint64_t& to, uint32_t& lost)
        {
            lost = 0;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool cut = false;
            if (s_p < s_o) { lost = (uint32_t)(s_o - s_p); s_p = s_o; cut = true; }
            from = s_p;
            const uint64_t avail = s_w - s_p;
            size_t n = avail < CHUNK_MAX ? (size_t)avail : CHUNK_MAX;
            ringGet(s_p, out, n);
            xSemaphoreGive(s_lock);
            size_t start = 0;
            if (cut) {   // s_o moved by bytes, not lines: drop the partial first line
                while (start < n && out[start] != '\n') ++start;
                if (start < n) ++start;
            }
            if (n < avail) {   // the cut at the back: up to the last whole line
                size_t e = n;
                while (e > start && out[e - 1] != '\n') --e;
                n = e;
            }
            to = from + n;
            if (start) { memmove(out, out + start, n - start); n -= start; }
            return n;
        }

        // The file's text: a lost-bytes note first when there is one, then the
        // chunk, base64 inside the contents API's JSON. nullptr: no memory.
        char* buildBody(const char* text, size_t n, uint32_t lost, const char* msg, size_t& len)
        {
            char note[96] = "";
            int nn = 0;
            if (lost) nn = snprintf(note, sizeof(note), "[push] %u bytes lost: the spool wrapped before they were pushed\n",
                                    (unsigned)lost);
            const size_t raw = nn + n;
            char* src = (char*)psMalloc(raw ? raw : 1);
            if (!src) return nullptr;
            memcpy(src, note, nn);
            memcpy(src + nn, text, n);
            const size_t b64 = 4 * ((raw + 2) / 3) + 1;
            const size_t cap = b64 + strlen(msg) + 64;
            char* body = (char*)psMalloc(cap);
            if (!body) { heap_caps_free(src); return nullptr; }
            int at = snprintf(body, cap, "{\"message\":\"%s\",\"content\":\"", msg);
            size_t olen = 0;
            const int e = mbedtls_base64_encode((unsigned char*)body + at, cap - at, &olen,
                                                (const unsigned char*)src, raw);
            heap_caps_free(src);
            if (e) { heap_caps_free(body); return nullptr; }
            at += (int)olen;
            at += snprintf(body + at, cap - at, "\"}");
            len = (size_t)at;
            return body;
        }

        // One PUT; the HTTP status, or -1 when it never got one (no internet, TLS).
        int put(const char* url, const char* tok, const char* body, size_t len, esp_err_t& err)
        {
            esp_http_client_config_t cfg = {};
            cfg.url            = url;
            cfg.cert_pem       = GH_ROOTS_PEM;
            cfg.timeout_ms     = TIMEOUT_MS;
            cfg.method         = HTTP_METHOD_PUT;
            // Above CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL: both buffers in PSRAM (GhOta.cpp).
            cfg.buffer_size    = 4608;
            cfg.buffer_size_tx = 4608;
            cfg.user_agent     = "MegaOpen/" MEGAOPEN_VERSION;
            esp_http_client_handle_t c = esp_http_client_init(&cfg);
            if (!c) { err = ESP_ERR_NO_MEM; return -1; }
            char auth[TOKEN_MAX + 16];
            snprintf(auth, sizeof(auth), "Bearer %s", tok);
            esp_http_client_set_header(c, "Authorization", auth);
            esp_http_client_set_header(c, "Accept", "application/vnd.github+json");
            esp_http_client_set_header(c, "X-GitHub-Api-Version", "2022-11-28");
            esp_http_client_set_header(c, "Content-Type", "application/json");
            esp_http_client_set_post_field(c, body, (int)len);
            err = esp_http_client_perform(c);
            const int st = err == ESP_OK ? esp_http_client_get_status_code(c) : -1;
            esp_http_client_cleanup(c);
            memset(auth, 0, sizeof(auth));
            return st;
        }

        void runPush()
        {
            char tok[TOKEN_MAX + 1], repo[REPO_MAX + 1], dev[16];
            portENTER_CRITICAL(&s_mux);
            memcpy(tok, s_tok, sizeof(tok));
            memcpy(repo, s_repo, sizeof(repo));
            memcpy(dev, s_dev, sizeof(dev));
            const uint32_t part = s_part;
            portEXIT_CRITICAL(&s_mux);
            if (!s_boot) {
                Preferences p;
                p.begin("sys", true);
                s_boot = p.getUInt("boots", 0);
                p.end();
            }

            char* text = (char*)psMalloc(CHUNK_MAX);
            if (!text) { setErr(State::Waiting, "no_memory", 0); return; }
            uint64_t from = 0, to = 0;
            uint32_t lost = 0;
            const size_t n = takeChunk(text, from, to, lost);
            if (!n && !lost) { heap_caps_free(text); setErr(State::Waiting, "", 0); return; }

            char msg[64], url[200];
            snprintf(msg, sizeof(msg), "%s boot %u part %u (" MEGAOPEN_VERSION ")", dev, (unsigned)s_boot, (unsigned)part);
            snprintf(url, sizeof(url), "https://api.github.com/repos/%s/contents/%s/boot-%05u/%04u.log",
                     repo, dev, (unsigned)s_boot, (unsigned)part);
            size_t len = 0;
            char* body = buildBody(text, n, lost, msg, len);
            heap_caps_free(text);
            if (!body) { setErr(State::Waiting, "no_memory", 0); return; }

            const uint32_t t0 = millis();
            esp_err_t err = ESP_OK;
            const int st = put(url, tok, body, len, err);
            heap_caps_free(body);
            memset(tok, 0, sizeof(tok));
            const uint32_t ms = millis() - t0;

            if (st == 201 || st == 200) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                if (to > s_p) s_p = to;
                const uint32_t queued = (uint32_t)(s_w - (s_p > s_o ? s_p : s_o));
                xSemaphoreGive(s_lock);
                portENTER_CRITICAL(&s_mux);
                s_state = State::Waiting;
                s_err[0] = 0;
                s_status = st;
                ++s_files;
                ++s_part;
                s_sent += (uint32_t)n;
                s_lost += lost;
                s_okAt = millis();
                s_okAt += !s_okAt;
                s_backoff = 0;
                s_nextAt = millis() + (queued >= BACKLOG_BYTES ? DRAIN_MS : PERIOD_MS);
                portEXIT_CRITICAL(&s_mux);
                LogBus::printf("[push] %s/boot-%05u/%04u.log: %u bytes in %u ms%s; %u queued; "
                               "internal heap %u, stack %u of %u unused",
                               dev, (unsigned)s_boot, (unsigned)part, (unsigned)n, (unsigned)ms,
                               lost ? " (after a lost stretch)" : "", (unsigned)queued, (unsigned)heapNow(),
                               (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned)App::Plan::LOGPUSH_STACK);
                return;
            }

            // A refusal that waiting will not fix stops the push until the page
            // saves new settings; the rest retry with a growing wait.
            const char* code;
            bool stop = false;
            switch (st) {
                case 401: code = "push_token";  stop = true; break;   // bad or expired token
                case 403: code = "push_denied"; break;                // no write right, or rate limit
                case 404: code = "push_repo";   stop = true; break;   // no such repo, or not this token's
                case 409: code = "push_conflict"; break;
                case 422: code = "push_exists"; break;                // the path is taken: next number
                case -1:  code = "push_net";    break;
                default:  code = "push_http";   break;
            }
            portENTER_CRITICAL(&s_mux);
            if (st == 422) ++s_part;
            s_backoff = s_backoff ? s_backoff * 2 : PERIOD_MS;
            if (s_backoff > BACKOFF_MAX_MS) s_backoff = BACKOFF_MAX_MS;
            s_nextAt = millis() + (st == 422 ? DRAIN_MS : s_backoff);
            portEXIT_CRITICAL(&s_mux);
            setErr(stop ? State::Stopped : State::Waiting, code, st);
            if (st < 0)
                LogBus::printf("[push] no answer (%s) after %u ms; retry in %u s", esp_err_to_name(err),
                               (unsigned)ms, (unsigned)(s_backoff / 1000));
            else
                LogBus::printf("[push] GitHub said %d (%s)%s", st, code,
                               stop ? ": stopped until the settings change" : "");
        }

        void task(void*)
        {
            {
                TlsJob::InPsram tls;
                runPush();
            }
            TlsJob::release();
            const uint32_t low = heap_caps_get_minimum_free_size(CAP_IN);
            portENTER_CRITICAL(&s_mux);
            s_heapMin = low;
            s_busy = false;
            if (s_state == State::Pushing) s_state = State::Waiting;
            portEXIT_CRITICAL(&s_mux);
            vTaskDelete(nullptr);
        }

        void load()
        {
            Preferences p;
            p.begin("logpush", true);
            const bool on = p.getBool("on", true);   // on by default: a token is the only step
            const String tok = p.getString("tok", "");
            String repo = p.getString("repo", "");
            p.end();
            if (!repo.length()) repo = REPO_DEFAULT;
            portENTER_CRITICAL(&s_mux);
            s_on = on;
            snprintf(s_tok, sizeof(s_tok), "%s", tok.c_str());
            snprintf(s_repo, sizeof(s_repo), "%s", repo.c_str());
            s_state = ready() ? State::Waiting : State::Off;
            portEXIT_CRITICAL(&s_mux);
        }
    }

    bool begin()
    {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return false;
        size_t size = SPOOL;
        s_buf = (uint8_t*)heap_caps_malloc(size, CAP_PS);
        if (!s_buf) { size = SPOOL_SMALL; s_buf = (uint8_t*)heap_caps_malloc(size, CAP_PS); }
        if (!s_buf) return false;
        s_size = size;
        snprintf(s_dev, sizeof(s_dev), "mo-%04X", (unsigned)(uint16_t)(ESP.getEfuseMac() & 0xFFFF));
        load();
        return true;
    }

    void sink(const char* line, int len, uint32_t ms)
    {
        if (!s_buf || len <= 0) return;
        char pre[16];
        const int pn = snprintf(pre, sizeof(pre), "%u.%03u ", (unsigned)(ms / 1000), (unsigned)(ms % 1000));
        const size_t n = (size_t)pn + (size_t)len + 1;
        if (n > s_size / 4) return;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_w + n - s_o > s_size) s_o = s_w + n - s_size;
        ringPut(s_w, pre, pn);
        ringPut(s_w + pn, line, len);
        ringPut(s_w + pn + len, "\n", 1);
        s_w += n;
        xSemaphoreGive(s_lock);
    }

    void tick(uint32_t now, bool online)
    {
        portENTER_CRITICAL(&s_mux);
        const bool go = ready() && online && !s_busy && s_state != State::Stopped;
        if (!online || !ready()) s_nextSet = false;
        else if (!s_nextSet) { s_nextAt = now + FIRST_MS; s_nextSet = true; }
        const bool due = go && s_nextSet && (int32_t)(now - s_nextAt) >= 0;
        portEXIT_CRITICAL(&s_mux);
        if (!due) return;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool any = s_w > s_p;
        xSemaphoreGive(s_lock);
        if (!any) { portENTER_CRITICAL(&s_mux); s_nextAt = now + PERIOD_MS; portEXIT_CRITICAL(&s_mux); return; }

        // Never the last of the internal heap: the next pass may find more.
        if (heapNow() < MIN_HEAP) {
            portENTER_CRITICAL(&s_mux);
            s_nextAt = now + PERIOD_MS;
            portEXIT_CRITICAL(&s_mux);
            setErr(State::Waiting, "push_heap", 0);
            return;
        }
        // GhOta's job holds TLS: try again on a later pass.
        if (!TlsJob::claim("logpush")) {
            portENTER_CRITICAL(&s_mux);
            s_nextAt = now + DRAIN_MS;
            portEXIT_CRITICAL(&s_mux);
            return;
        }
        portENTER_CRITICAL(&s_mux);
        s_busy = true;
        s_state = State::Pushing;
        portEXIT_CRITICAL(&s_mux);
        if (xTaskCreatePinnedToCore(task, "logpush", App::Plan::LOGPUSH_STACK, nullptr,
                                    App::Plan::LOGPUSH_PRIO, nullptr,
                                    App::core(App::Plan::LOGPUSH_CORE)) != pdPASS) {
            TlsJob::release();
            portENTER_CRITICAL(&s_mux);
            s_busy = false;
            s_nextAt = now + PERIOD_MS;
            portEXIT_CRITICAL(&s_mux);
            setErr(State::Waiting, "no_memory", 0);
        }
    }

    const char* stateId(State s)
    {
        switch (s) {
            case State::Off:     return "off";
            case State::Waiting: return "waiting";
            case State::Pushing: return "pushing";
            case State::Stopped: return "stopped";
        }
        return "?";
    }

    void view(View& v)
    {
        uint32_t queued = 0;
        if (s_lock) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            queued = (uint32_t)(s_w - (s_p > s_o ? s_p : s_o));
            xSemaphoreGive(s_lock);
        }
        const uint32_t now = millis();
        portENTER_CRITICAL(&s_mux);
        v.state = s_state;
        v.on = s_on;
        v.hasToken = s_tok[0] != 0;
        memcpy(v.repo, s_repo, sizeof(v.repo));
        memcpy(v.device, s_dev, sizeof(v.device));
        memcpy(v.err, s_err, sizeof(v.err));
        v.lastStatus = s_status;
        v.files = s_files;
        v.sentBytes = s_sent;
        v.lostBytes = s_lost;
        v.queuedBytes = queued;
        v.spoolBytes = (uint32_t)s_size;
        v.lastOkAgoMs = s_okAt ? now - s_okAt : 0;
        v.nextInMs = s_nextSet && (int32_t)(s_nextAt - now) > 0 ? s_nextAt - now : 0;
        v.heapMin = s_heapMin;
        portEXIT_CRITICAL(&s_mux);
    }

    const char* set(const bool* on, const char* token, const char* repo)
    {
        if (token && token[0] && !tokenOk(token)) return "push_token_bad";
        if (repo && !repoOk(repo)) return "push_repo_bad";
        Preferences p;
        if (!p.begin("logpush", false)) return "nvs";
        if (on) p.putBool("on", *on);
        if (token && token[0]) p.putString("tok", token);
        if (repo) p.putString("repo", repo);
        p.end();
        load();
        // New settings: a stop is lifted, the next push is soon.
        portENTER_CRITICAL(&s_mux);
        if (s_state == State::Stopped || (ready() && s_state == State::Off)) s_state = State::Waiting;
        s_err[0] = 0;
        s_backoff = 0;
        s_nextSet = false;
        portEXIT_CRITICAL(&s_mux);
        LogBus::printf("[push] settings saved: %s, repo %s, token %s", s_on ? "on" : "off", s_repo,
                       s_tok[0] ? "set" : "none");
        return nullptr;
    }

    void pushSoon()
    {
        portENTER_CRITICAL(&s_mux);
        if (s_state == State::Stopped) s_state = State::Waiting;
        s_nextAt = millis();
        s_nextSet = true;
        s_backoff = 0;
        portEXIT_CRITICAL(&s_mux);
    }
}
#endif
