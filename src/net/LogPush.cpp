#include "LogPush.h"
#if FEATURE_LOG_PUSH
#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include "../link/UsbLink.h"
#include <mbedtls/base64.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <atomic>
#include "GhRoots.h"
#include "TlsJob.h"
#include "../Version.h"
#include "../app/TaskPlan.h"
#include "../utils/LogBus.h"
#if FEATURE_LOGFS
#include <LittleFS.h>
#include "../utils/LogFile.h"
#endif
#if FEATURE_CAPTURE
#include <time.h>
#include "../capture/CanRecorder.h"
#include "../utils/WallClock.h"
#endif

namespace LogPush
{
    namespace
    {
        constexpr int      TIMEOUT_MS = 20000;
        constexpr uint32_t FIRST_MS   = 20000;   // after the link comes up: let the boot settle
        constexpr uint32_t DRAIN_MS   = 6000;    // a backlog waits: the next file soon, not back to back
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

        // The CAN part (NVS "can", "canMB"; progress "cnum"/"coff": the file
        // being sent and how far; "cday"/"cused": the day and its bytes).
        bool     s_can = false;
        uint32_t s_canMB = CAN_MB_DEFAULT;
        uint32_t s_cnum = 0, s_coff = 0, s_cday = 0, s_cused = 0;
        bool     s_canBudget = false;
        bool     s_canStarted = false;  // the recorder was started for it this boot
        uint32_t s_spoolAt = 0;         // millis() of the last spool push
        std::atomic<uint32_t> s_noAnswer{0};   // wifiNoAnswer()

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

        // The same PUT through the app on the USB link (src/link/UsbLink.h):
        // the request is ours, token and all; the app only moves the bytes.
        int putLink(const char* url, const char* tok, const char* body, size_t len, esp_err_t& err)
        {
            char auth[TOKEN_MAX + 16];
            snprintf(auth, sizeof(auth), "Bearer %s", tok);
            const char* hdr[] = {"Authorization", auth, "Accept", "application/vnd.github+json",
                                 "X-GitHub-Api-Version", "2022-11-28", "Content-Type", "application/json",
                                 "User-Agent", "MegaOpen/" MEGAOPEN_VERSION, nullptr};
            UsbLink::NetHead head;
            const char* why = "";
            const bool ok = UsbLink::netOpen("PUT", url, hdr, (const uint8_t*)body, len, TIMEOUT_MS, head, why);
            memset(auth, 0, sizeof(auth));
            if (!ok) {
                LOGI("push", "through the app on USB: %s", why);
                err = ESP_FAIL;
                return -1;
            }
            // GitHub's answer (the new file's JSON) is not needed: drained.
            uint8_t sink[256];
            while (UsbLink::netRead(sink, sizeof(sink), TIMEOUT_MS) > 0) {}
            UsbLink::netClose();
            err = ESP_OK;
            return head.status;
        }

        // One PUT; the HTTP status, or -1 when it never got one (no internet, TLS).
        int put(const char* url, const char* tok, const char* body, size_t len, esp_err_t& err)
        {
            if (UsbLink::netAvailable()) return putLink(url, tok, body, len, err);
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
            if (st > 0) s_noAnswer = 0;
            else if (err != ESP_ERR_NO_MEM) ++s_noAnswer;   // our memory is not the link's fault
            return st;
        }

        // A refusal that waiting will not fix stops the push until the page
        // saves new settings; the rest retry with a growing wait. 422 (the path
        // is taken) moves *part on and tries again soon.
        void failed(int st, esp_err_t err, uint32_t ms, uint32_t* part)
        {
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
            if (st == 422) ++*part;
            s_backoff = s_backoff ? s_backoff * 2 : PERIOD_MS;
            if (s_backoff > BACKOFF_MAX_MS) s_backoff = BACKOFF_MAX_MS;
            s_nextAt = millis() + (st == 422 ? DRAIN_MS : s_backoff);
            portEXIT_CRITICAL(&s_mux);
            setErr(stop ? State::Stopped : State::Waiting, code, st);
            if (st < 0)
                LOGI("push", "no answer (%s) after %u ms; retry in %u s", esp_err_to_name(err),
                               (unsigned)ms, (unsigned)(s_backoff / 1000));
            else
                LOGI("push", "GitHub said %d (%s)%s", st, code,
                               stop ? ": stopped until the settings change" : "");
        }

        // One file to <repo>/<dev>/<rel>; the HTTP status (-1: none).
        int putFile(const char* repo, const char* tok, const char* rel, const char* msg,
                    const char* data, size_t n, uint32_t lost, esp_err_t& err, uint32_t& ms)
        {
            char url[220];
            snprintf(url, sizeof(url), "https://api.github.com/repos/%s/contents/%s/%s", repo, s_dev, rel);
            size_t len = 0;
            char* body = buildBody(data, n, lost, msg, len);
            if (!body) { err = ESP_ERR_NO_MEM; ms = 0; return -1; }
            const uint32_t t0 = millis();
            const int st = put(url, tok, body, len, err);
            ms = millis() - t0;
            heap_caps_free(body);
            return st;
        }

        // A good PUT that was not the spool's: the next piece soon.
        void sentPast(int st, size_t n)
        {
            portENTER_CRITICAL(&s_mux);
            s_state = State::Waiting;
            s_err[0] = 0;
            s_status = st;
            ++s_files;
            s_sent += (uint32_t)n;
            s_okAt = millis();
            s_okAt += !s_okAt;
            s_backoff = 0;
            s_nextAt = millis() + DRAIN_MS;
            portEXIT_CRITICAL(&s_mux);
        }

        // "12.345 [x] ..." -> 12345; false when the line has no stamp.
        bool lineMs(const char* p, const char* end, uint32_t& ms)
        {
            uint32_t s = 0;
            const char* q = p;
            while (q < end && *q >= '0' && *q <= '9') s = s * 10 + (uint32_t)(*q++ - '0');
            if (q == p || q + 4 > end || *q != '.') return false;
            uint32_t f = 0;
            for (int i = 1; i <= 3; ++i) {
                if (q[i] < '0' || q[i] > '9') return false;
                f = f * 10 + (uint32_t)(q[i] - '0');
            }
            ms = s * 1000 + f;
            return true;
        }

#if FEATURE_LOGFS
        // ---- what earlier boots left on flash -------------------------------
        // A push that never came (no internet, a power cut, a crash) leaves the
        // lines in the flash log only (src/utils/LogFile.h), and a crash in the
        // coredump partition (main.cpp copies it to /crash/). Before the spool,
        // each job sends one piece of that: a coredump, or a chunk of an earlier
        // boot's log from the line after the last one pushed (NVS "pb"/"pms"; all
        // of it for a boot that pushed nothing), as boot-N/flash-PP.log. NVS
        // "rec" is the newest boot sent in full; nothing older is sent again.
        struct Past { uint32_t boot, fromMs; int8_t first, last; };   // files first..last, oldest first
        Past     s_past[LogFile::FILES];
        int      s_pastN = -1;          // -1: not looked at yet this boot
        int      s_pastI = 0;           // the boot being sent
        int      s_pastFile = -1;       // its file being sent (number at the scan), -1: not started
        size_t   s_pastOff = 0;
        uint32_t s_pastPart = 0;
        uint32_t s_rotAtScan = 0;
        bool     s_crashDone = false;
        char*    s_fbuf = nullptr;      // one flash log file, PSRAM
        size_t   s_flen = 0;
        int      s_fnum = -1;           // which file s_fbuf holds (its number at s_frot), -1 none
        uint32_t s_frot = 0;

        constexpr size_t FBUF = LogFile::FILE_MAX + 2 * LogFile::FLUSH_BYTES + 1;

        bool loadFile(int n)
        {
            if (s_fnum == n && s_frot == LogFile::rotations()) return true;
            s_fnum = -1;
            s_frot = LogFile::rotations();
            if (!s_fbuf) s_fbuf = (char*)heap_caps_malloc(FBUF, CAP_PS);
            if (!s_fbuf) return false;
            char path[16];
            snprintf(path, sizeof(path), "/log/%d.log", n);
            File f = LittleFS.open(path, "r");
            if (!f) return false;
            s_flen = f.read((uint8_t*)s_fbuf, FBUF - 1);
            f.close();
            s_fbuf[s_flen] = 0;
            s_fnum = n;
            return true;
        }

        // The boots= of a [boot] line in the loaded file, 0 none.
        uint32_t bootIn()
        {
            const char* b = strstr(s_fbuf, "[boot] build=");
            if (!b) return 0;
            const char* e = strchr(b, '\n');
            const char* k = strstr(b, " boots=");
            if (!k || (e && k > e)) return 0;
            return (uint32_t)strtoul(k + 7, nullptr, 10);
        }

        void scanPast()
        {
            Preferences p;
            p.begin("logpush", true);
            const uint32_t rec = p.getUInt("rec", 0), pb = p.getUInt("pb", 0), pms = p.getUInt("pms", 0);
            p.end();
            s_rotAtScan = LogFile::rotations();
            s_pastN = 0;
            int cur = -1;
            // The oldest files may have lost their boot's first file (a long
            // drive rotates past it). Each boot starts a file of its own, so a
            // headless run just before boot B's first file is boot B - 1's tail.
            int hFirst = -1, hLast = -1;
            bool known = false;
            for (int n = LogFile::FILES - 1; n >= 1; --n) {
                if (!loadFile(n)) continue;
                const uint32_t b = bootIn();
                if (!b && !known) {
                    if (hFirst < 0) hFirst = n;
                    hLast = n;
                    continue;
                }
                if (b && !known) {
                    known = true;
                    const uint32_t t = b - 1;
                    if (hFirst >= 0 && t > rec && t < s_boot && s_pastN < (int)LogFile::FILES)
                        s_past[s_pastN++] = {t, t == pb ? pms + 1 : 0, (int8_t)hFirst, (int8_t)hLast};
                }
                if (b) {
                    cur = -1;
                    if (b > rec && b < s_boot && s_pastN < (int)LogFile::FILES) {
                        cur = s_pastN++;
                        s_past[cur] = {b, b == pb ? pms + 1 : 0, (int8_t)n, (int8_t)n};
                    }
                } else if (cur >= 0) {
                    s_past[cur].last = (int8_t)n;   // the same boot, shifted mid-run
                }
            }
            s_fnum = -1;
            for (int i = 0; i < s_pastN; ++i)
                LOGI("push", "boot %u: not all pushed, sending from flash (from %u ms, %d file(s))",
                               (unsigned)s_past[i].boot, (unsigned)s_past[i].fromMs,
                               s_past[i].first - s_past[i].last + 1);
        }

        void pastDone(uint32_t boot)
        {
            Preferences p;
            p.begin("logpush", false);
            p.putUInt("rec", boot);
            p.end();
            ++s_pastI;
            s_pastFile = -1;
            s_pastPart = 0;
        }

        // A coredump is the board's RAM: the token and the WiFi passwords are
        // in it as text (the car's dumps carried "Bearer github_pat_..." to
        // the log repository, 1.6.0-rc5). Each one is overwritten with '*'
        // before the dump leaves, as is any other github_pat_ / ghp_ run.
        void blank(char* d, size_t n, const char* secret)
        {
            const size_t k = secret ? strlen(secret) : 0;
            if (k < 4 || k > n) return;
            for (size_t i = 0; i + k <= n; ++i)
                if (d[i] == secret[0] && !memcmp(d + i, secret, k)) { memset(d + i, '*', k); i += k - 1; }
        }
        void redact(char* d, size_t n, const char* tok)
        {
            blank(d, n, tok);
            static const char* const PASS[][2] = {{"megaopen", "pass"}, {"megaopen", "apPass"}, {"net2", "pass"}};
            for (const auto& kv : PASS) {
                Preferences p;
                if (!p.begin(kv[0], true)) continue;
                const String s = p.getString(kv[1], "");
                p.end();
                blank(d, n, s.c_str());
            }
            static const char* const PREFIX[] = {"github_pat_", "ghp_"};
            for (const char* pre : PREFIX) {
                const size_t k = strlen(pre);
                for (size_t i = 0; i + k <= n; ++i) {
                    if (memcmp(d + i, pre, k)) continue;
                    size_t j = i + k;
                    while (j < n && (isalnum((unsigned char)d[j]) || d[j] == '_')) d[j++] = '*';
                    i = j;
                }
            }
        }

        // A coredump copied by main.cpp, if one is there: true when it was tried.
        bool crashStep(const char* repo, const char* tok)
        {
            if (s_crashDone) return false;
            if (!LittleFS.exists("/crash")) { s_crashDone = true; return false; }
            File d = LittleFS.open("/crash");
            File f = d ? d.openNextFile() : File();
            if (!f) { s_crashDone = true; return false; }
            char path[40], rel[48];
            const char* nm = f.name();
            const char* base = strrchr(nm, '/') ? strrchr(nm, '/') + 1 : nm;
            snprintf(path, sizeof(path), "/crash/%s", base);
            const uint32_t boot = (uint32_t)strtoul(base, nullptr, 10);
            const size_t n = f.size();
            char* data = n && n <= CHUNK_MAX ? (char*)psMalloc(n) : nullptr;
            const size_t got = data ? f.read((uint8_t*)data, n) : 0;
            f.close();
            d.close();
            if (got != n || !data) {
                if (data) heap_caps_free(data);
                LittleFS.remove(path);
                LOGI("push", "%s: unreadable, removed", path);
                return false;
            }
            redact(data, n, tok);
            char msg[64];
            snprintf(msg, sizeof(msg), "%s boot %u coredump (" MEGAOPEN_VERSION ")", s_dev, (unsigned)boot);
            snprintf(rel, sizeof(rel), "boot-%05u/coredump.bin", (unsigned)boot);
            esp_err_t err = ESP_OK;
            uint32_t ms = 0, dummy = 0;
            const int st = putFile(repo, tok, rel, msg, data, n, 0, err, ms);
            heap_caps_free(data);
            if (st == 201 || st == 200 || st == 422) {   // 422: sent before, the reply was lost
                LittleFS.remove(path);
                sentPast(st, n);
                LOGI("push", "%s/%s: %u bytes in %u ms", s_dev, rel, (unsigned)n, (unsigned)ms);
            } else {
                failed(st, err, ms, &dummy);
            }
            return true;
        }

        // The next chunk of an earlier boot's flash log: true when a PUT was tried.
        bool pastStep(const char* repo, const char* tok)
        {
            if (s_pastN < 0) scanPast();
            char* text = nullptr;
            while (s_pastI < s_pastN) {
                Past& g = s_past[s_pastI];
                if (s_pastFile < 0) { s_pastFile = g.first; s_pastOff = 0; }
                if (s_pastFile < g.last) { pastDone(g.boot); continue; }
                const int now = s_pastFile + (int)(LogFile::rotations() - s_rotAtScan);
                if (now >= LogFile::FILES || !loadFile(now)) {   // rotated away meanwhile
                    LOGI("push", "boot %u: /log file gone before it was sent", (unsigned)g.boot);
                    pastDone(g.boot);
                    continue;
                }
                if (!text) text = (char*)psMalloc(CHUNK_MAX);
                if (!text) return false;
                // Whole lines from s_pastOff, the ones after fromMs, up to CHUNK_MAX.
                size_t n = 0, off = s_pastOff;
                while (off < s_flen) {
                    const char* ln = s_fbuf + off;
                    const char* nl = (const char*)memchr(ln, '\n', s_flen - off);
                    const size_t ll = nl ? (size_t)(nl - ln) + 1 : s_flen - off;
                    if (n + ll > CHUNK_MAX) break;
                    uint32_t ms = 0;
                    if (!lineMs(ln, ln + ll, ms) || ms >= g.fromMs) { memcpy(text + n, ln, ll); n += ll; }
                    off += ll;
                }
                const bool fileDone = off >= s_flen;
                if (!n) {
                    s_pastOff = off;
                    if (fileDone) { --s_pastFile; s_pastOff = 0; }
                    continue;
                }
                char msg[64], rel[48];
                snprintf(msg, sizeof(msg), "%s boot %u from flash %u (" MEGAOPEN_VERSION ")", s_dev,
                         (unsigned)g.boot, (unsigned)s_pastPart);
                snprintf(rel, sizeof(rel), "boot-%05u/flash-%02u.log", (unsigned)g.boot, (unsigned)s_pastPart);
                esp_err_t err = ESP_OK;
                uint32_t ms = 0;
                const int st = putFile(repo, tok, rel, msg, text, n, 0, err, ms);
                heap_caps_free(text);
                if (st == 201 || st == 200) {
                    ++s_pastPart;
                    s_pastOff = off;
                    if (fileDone) { --s_pastFile; s_pastOff = 0; }
                    sentPast(st, n);
                    LOGI("push", "%s/%s: %u bytes in %u ms", s_dev, rel, (unsigned)n, (unsigned)ms);
                } else {
                    failed(st, err, ms, &s_pastPart);
                }
                return true;
            }
            if (text) heap_caps_free(text);
            if (s_fbuf) { heap_caps_free(s_fbuf); s_fbuf = nullptr; s_fnum = -1; }
            return false;
        }
#endif

#if FEATURE_CAPTURE
        // ---- the CAN recording ------------------------------------------------
        // The recording's files, oldest first, in PSRAM; how many (0: none or no memory).
        constexpr size_t CAP_LIST = 64;
        Capture::FileInfo* s_capList = nullptr;
        size_t capFiles()
        {
            if (!s_capList) s_capList = (Capture::FileInfo*)psMalloc(CAP_LIST * sizeof(Capture::FileInfo));
            if (!s_capList) return 0;
            const size_t total = Capture::list(s_capList, CAP_LIST);
            return total < CAP_LIST ? total : CAP_LIST;
        }

        uint32_t today()
        {
            return WallClock::known() ? (uint32_t)(time(nullptr) / 86400) : s_cday;
        }

        // The next part of the oldest closed recording file not sent yet: true
        // when a PUT was tried.
        bool canStep(const char* repo, const char* tok)
        {
            if (!s_can) return false;
            const uint32_t day = today();
            if (day != s_cday) { s_cday = day; s_cused = 0; }
            s_canBudget = (uint64_t)s_cused >= (uint64_t)s_canMB * 1024 * 1024;
            if (s_canBudget) return false;
            const size_t n = capFiles();
            const Capture::FileInfo* files = s_capList;
            const Capture::FileInfo* f = nullptr;
            for (size_t i = 0; i < n && !f; ++i) {
                if (files[i].current) continue;
                if (files[i].num > s_cnum || (files[i].num == s_cnum && s_coff < files[i].size)) f = &files[i];
            }
            if (!f) return false;
            if (f->num != s_cnum) { s_cnum = f->num; s_coff = 0; }
            char path[32];
            if (Capture::beginRead(f->num, path, sizeof(path)) != Capture::Res::OK) return false;
            const uint32_t want = f->size - s_coff < CAN_PART ? f->size - s_coff : CAN_PART;
            char* data = (char*)psMalloc(want ? want : 1);
            size_t got = 0;
            if (data) {
                File fl = LittleFS.open(path, "r");
                if (fl && fl.seek(s_coff)) got = fl.read((uint8_t*)data, want);
                if (fl) fl.close();
            }
            Capture::endRead(f->num);
            if (!data || got != want) {
                if (data) heap_caps_free(data);
                LOGI("push", "cap %04u: read failed at %u, skipped", (unsigned)f->num, (unsigned)s_coff);
                s_coff = f->size;
                return false;
            }
            const uint32_t part = s_coff / CAN_PART;
            char msg[72], rel[64];
            snprintf(msg, sizeof(msg), "%s cap %04u part %u (" MEGAOPEN_VERSION ")", s_dev, (unsigned)f->num, (unsigned)part);
            snprintf(rel, sizeof(rel), "cap/%04u/%04u-%02u.bin", (unsigned)f->session, (unsigned)f->num, (unsigned)part);
            esp_err_t err = ESP_OK;
            uint32_t ms = 0, dummy = 0;
            const int st = putFile(repo, tok, rel, msg, data, want, 0, err, ms);
            heap_caps_free(data);
            if (st == 201 || st == 200 || st == 422) {   // 422: sent before, the reply was lost
                s_coff += want;
                s_cused += want;
                Preferences p;
                p.begin("logpush", false);
                p.putUInt("cnum", s_cnum);
                p.putUInt("coff", s_coff);
                p.putUInt("cday", s_cday);
                p.putUInt("cused", s_cused);
                p.end();
                sentPast(st, want);
                LOGI("push", "%s/%s: %u bytes in %u ms; today %u kB of %u MB", s_dev, rel,
                               (unsigned)want, (unsigned)ms, (unsigned)(s_cused / 1024), (unsigned)s_canMB);
            } else {
                failed(st, err, ms, &dummy);
            }
            return true;
        }
#endif

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

#if FEATURE_LOGFS
            // Earlier boots first: a coredump, then what their flash log kept.
            if (LogFile::fsMounted() && (crashStep(repo, tok) || pastStep(repo, tok))) {
                memset(tok, 0, sizeof(tok));
                return;
            }
#endif
#if FEATURE_CAPTURE
            // The log every PERIOD_MS; the CAN recording in between.
            if (s_can && s_spoolAt && millis() - s_spoolAt < PERIOD_MS) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                const bool backlog = s_w - (s_p > s_o ? s_p : s_o) >= BACKLOG_BYTES;
                xSemaphoreGive(s_lock);
                if (!backlog) {
                    if (!canStep(repo, tok)) {   // nothing to send: the log at its time
                        portENTER_CRITICAL(&s_mux);
                        s_nextAt = s_spoolAt + PERIOD_MS;
                        portEXIT_CRITICAL(&s_mux);
                    }
                    memset(tok, 0, sizeof(tok));
                    return;
                }
            }
#endif
            char* text = (char*)psMalloc(CHUNK_MAX);
            if (!text) { setErr(State::Waiting, "no_memory", 0); return; }
            uint64_t from = 0, to = 0;
            uint32_t lost = 0;
            const size_t n = takeChunk(text, from, to, lost);
            if (!n && !lost) { heap_caps_free(text); setErr(State::Waiting, "", 0); return; }
            // The stamp of its last line: after a reboot, the flash log is sent from there on.
            uint32_t lastMs = 0;
            if (n) {
                size_t i = n - 1;
                while (i > 0 && text[i - 1] != '\n') --i;
                lineMs(text + i, text + n, lastMs);
            }

            char msg[64], rel[40];
            snprintf(msg, sizeof(msg), "%s boot %u part %u (" MEGAOPEN_VERSION ")", dev, (unsigned)s_boot, (unsigned)part);
            snprintf(rel, sizeof(rel), "boot-%05u/%04u.log", (unsigned)s_boot, (unsigned)part);
            esp_err_t err = ESP_OK;
            uint32_t ms = 0;
            const int st = putFile(repo, tok, rel, msg, text, n, lost, err, ms);
            heap_caps_free(text);
            memset(tok, 0, sizeof(tok));

            if (st == 201 || st == 200) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                if (to > s_p) s_p = to;
                const uint32_t queued = (uint32_t)(s_w - (s_p > s_o ? s_p : s_o));
                xSemaphoreGive(s_lock);
                s_spoolAt = millis();
                if (lastMs) {
                    Preferences p;
                    p.begin("logpush", false);
                    p.putUInt("pb", s_boot);
                    p.putUInt("pms", lastMs);
                    p.end();
                }
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
                // With the CAN recording on, the jobs in between send it.
                s_nextAt = millis() + (queued >= BACKLOG_BYTES || s_can ? DRAIN_MS : PERIOD_MS);
                portEXIT_CRITICAL(&s_mux);
                LOGI("push", "%s/%s: %u bytes in %u ms%s; %u queued; "
                               "internal heap %u, stack %u of %u unused",
                               dev, rel, (unsigned)n, (unsigned)ms,
                               lost ? " (after a lost stretch)" : "", (unsigned)queued, (unsigned)heapNow(),
                               (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned)App::Plan::LOGPUSH_STACK);
                return;
            }
            failed(st, err, ms, &s_part);
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
            const bool can = p.getBool("can", false);
            const uint32_t canMB = p.getUInt("canMB", CAN_MB_DEFAULT);
            const uint32_t cnum = p.getUInt("cnum", 0), coff = p.getUInt("coff", 0);
            const uint32_t cday = p.getUInt("cday", 0), cused = p.getUInt("cused", 0);
            p.end();
            s_can = can;
            s_canMB = canMB;
            s_cnum = cnum; s_coff = coff; s_cday = cday; s_cused = cused;
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
#if FEATURE_CAPTURE
        // Sending the recording means recording, offline too (the garage).
        if (s_can && !s_canStarted) {
            const Capture::Status cs = Capture::status();
            if (cs.ready) {
                s_canStarted = true;
                if (!cs.recording) {
                    Capture::start();
                    LOGI("push", "CAN to GitHub is on: recording started");
                }
            }
        }
#endif
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

    uint32_t wifiNoAnswer() { return s_noAnswer; }

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
        v.canOn = s_can;
        v.canMB = s_canMB;
        v.canToday = s_cused;
        v.canFile = s_can ? s_cnum : 0;
        v.canBudget = s_canBudget;
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
        LOGI("push", "settings saved: %s, repo %s, token %s", s_on ? "on" : "off", s_repo,
                       s_tok[0] ? "set" : "none");
        return nullptr;
    }

    const char* setCan(const bool* on, const uint32_t* mb)
    {
        if (mb && (*mb < 1 || *mb > CAN_MB_MAX)) return "push_can_mb";
        Preferences p;
        if (!p.begin("logpush", false)) return "nvs";
        if (on) p.putBool("can", *on);
        if (mb) p.putUInt("canMB", *mb);
        if (on && *on && !s_can) {
            // From now on: what was recorded before is not sent.
            uint32_t newest = 0;
#if FEATURE_CAPTURE
            const size_t n = capFiles();
            for (size_t i = 0; i < n; ++i)
                if (!s_capList[i].current && s_capList[i].num > newest) newest = s_capList[i].num;
#endif
            p.putUInt("cnum", newest);
            p.putUInt("coff", 0xFFFFFFFFu);
        }
        p.end();
        load();
        s_canStarted = false;
        LOGI("push", "CAN to GitHub: %s, %u MB a day", s_can ? "on" : "off", (unsigned)s_canMB);
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
