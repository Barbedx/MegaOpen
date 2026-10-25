#include "GhOta.h"
#if FEATURE_GH_OTA
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <mbedtls/md.h>
#include "GhRelease.h"
#include "GhRoots.h"
#include "TlsJob.h"
#include "../Version.h"
#include "../app/TaskPlan.h"
#include "../utils/LogBus.h"
#include "../vehicle/VehicleState.h"

#ifndef GH_OTA_REPO
#define GH_OTA_REPO "Barbedx/MegaOpen"
#endif

namespace GhOta
{
    namespace
    {
        constexpr const char* LATEST_URL = "https://api.github.com/repos/" GH_OTA_REPO "/releases/latest";
        constexpr int      HOPS       = 5;       // github.com -> the asset CDN is one
        constexpr int      TIMEOUT_MS = 15000;
        // The download's read and flash-write unit, in INTERNAL RAM: a source in
        // internal RAM is written straight to flash, one in PSRAM is bounced
        // through small stack copies. 1 kB, where Arduino's Update would hold
        // a 4 kB sector buffer internal for the whole download.
        constexpr size_t   CHUNK      = 1024;
        constexpr size_t   SUMS_MAX   = 4096;    // three envs x two images: ~600 bytes
        constexpr uint32_t PAGE_SEES_REBOOT_MS = 1500;
        constexpr uint32_t WAIT_POLL_MS = 500;     // Written: how often the gates are read

        portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
        View s_v;                              // under s_mux
        bool s_busy = false;                   // under s_mux: a job task exists
        GhRelease::Release s_rel;              // the last check's pick; the job owns it
        volatile bool s_rebootAsked = false;   // the page confirmed, Written only
        const Vehicle::State* s_car = nullptr;
        // ElegantOTA's session: open, and when it last showed signs of life.
        volatile bool     s_local   = false;
        volatile uint32_t s_localAt = 0;

        // Speed fresh and exactly 0: the one state the reboot may take by itself.
        bool parked()
        {
            const uint32_t now = millis();
            return s_car && s_car->fresh(Vehicle::SIG_SPEED, now, SPEED_FRESH_MS) &&
                   s_car->get(Vehicle::SIG_SPEED) == 0;
        }

        enum class Job : uint8_t { Check, Install };

        uint32_t heapNow() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

        void setPhase(Phase p)
        {
            portENTER_CRITICAL(&s_mux);
            s_v.phase = p;
            portEXIT_CRITICAL(&s_mux);
        }

        void sampleHeap()
        {
            const uint32_t h = heapNow();
            portENTER_CRITICAL(&s_mux);
            if (h < s_v.heapMin) s_v.heapMin = h;
            portEXIT_CRITICAL(&s_mux);
        }

        // Failed: `code` (with its args, "gh_http 503") goes to the page, which
        // has the words (web/i18n e.<code>); the detail, in English, to the log.
        void fail(const char* code, const char* fmt = nullptr, ...)
        {
            char d[sizeof(s_v.err) + 64] = "";
            if (fmt) {
                va_list ap;
                va_start(ap, fmt);
                vsnprintf(d, sizeof(d), fmt, ap);
                va_end(ap);
            }
            portENTER_CRITICAL(&s_mux);
            s_v.phase = Phase::Failed;
            snprintf(s_v.err, sizeof(s_v.err), "%s", code);
            portEXIT_CRITICAL(&s_mux);
            LogBus::printf("[ota] github: %s%s%s", code, *d ? ": " : "", d);
        }

        // mbedTLS itself goes to PSRAM for the job (TlsJob::InPsram).
        // Everything else the job allocates and could keep in PSRAM: the
        // download chunk, SHA256SUMS, the JSON pools. Internal RAM when there
        // is no PSRAM (heap_caps_free frees either).
        constexpr uint32_t CAP_PS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        constexpr uint32_t CAP_IN = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        void* psMalloc(size_t n) { return heap_caps_malloc_prefer(n, 2, CAP_PS, CAP_IN); }

        struct PsramJson final : ArduinoJson::Allocator {
            void* allocate(size_t n) override { return psMalloc(n); }
            void  deallocate(void* p) override { heap_caps_free(p); }
            void* reallocate(void* p, size_t n) override { return heap_caps_realloc_prefer(p, n, 2, CAP_PS, CAP_IN); }
        };
        PsramJson s_json;

        // Internal heap at the points of a job, for the log: what the task, the
        // client and the handshake each cost.
        void stage(uint32_t View::*at)
        {
            const uint32_t h = heapNow();
            portENTER_CRITICAL(&s_mux);
            s_v.*at = h;
            if (h < s_v.heapMin) s_v.heapMin = h;
            portEXIT_CRITICAL(&s_mux);
        }

        // The host of a URL, for a log line: at most `n` bytes.
        void hostOf(const char* url, char* out, size_t n)
        {
            const char* h = strstr(url, "://");
            h = h ? h + 3 : url;
            snprintf(out, n, "%.*s", (int)strcspn(h, "/?#"), h);
        }

        // A GET that follows redirects (a release asset is a 302 to the CDN,
        // with a ~1 kB Location: hence the buffers). Every URL, the first and
        // each redirect's, must be GitHub over https (GhRelease::trustedUrl).
        // The open client, headers read, status 200; nullptr after fail().
        esp_http_client_handle_t get(const char* url, bool api, int64_t& len)
        {
            if (!GhRelease::trustedUrl(url)) {
                char h[48];
                hostOf(url, h, sizeof(h));
                fail("gh_host", "refused %s: not GitHub over https", h);
                return nullptr;
            }
            esp_http_client_config_t cfg = {};
            cfg.url            = url;
            cfg.cert_pem       = GH_ROOTS_PEM;
            cfg.timeout_ms     = TIMEOUT_MS;
            // Above CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (4096), so the client's
            // malloc puts both in PSRAM; at 2048 they took 4 kB of internal heap.
            cfg.buffer_size    = 4608;
            cfg.buffer_size_tx = 4608;
            cfg.user_agent     = "MegaOpen/" MEGAOPEN_VERSION;   // the API refuses none
            esp_http_client_handle_t c = esp_http_client_init(&cfg);
            if (!c) { fail("no_memory", "for the HTTP client"); return nullptr; }
            stage(&View::heapClient);
            if (api) esp_http_client_set_header(c, "Accept", "application/vnd.github+json");

            int hop = 0;
            for (; hop < HOPS; ++hop) {
                const esp_err_t e = esp_http_client_open(c, 0);
                if (e != ESP_OK) {
                    fail("gh_connect", "%s: no internet, or TLS", esp_err_to_name(e));
                    break;
                }
                stage(&View::heapTls);
                len = esp_http_client_fetch_headers(c);
                const int st = esp_http_client_get_status_code(c);
                if (st == 200) return c;
                if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) {
                    // A new host closes the connection inside set_url.
                    esp_http_client_set_redirection(c);
                    esp_http_client_close(c);
                    // Where it now points, read back: a Location cut short
                    // (split across reads) or off GitHub stops here.
                    constexpr size_t URL_MAX = 2048;
                    char* u = (char*)psMalloc(URL_MAX);
                    if (!u) { fail("no_memory", "for the redirect"); break; }
                    const bool got = esp_http_client_get_url(c, u, URL_MAX) == ESP_OK;
                    char h[48];
                    hostOf(got ? u : "", h, sizeof(h));
                    LogBus::printf("[ota] github: %d -> %s (%u bytes)", st, h, got ? (unsigned)strlen(u) : 0u);
                    const bool ok = got && GhRelease::trustedUrl(u);
                    heap_caps_free(u);
                    if (!ok) { fail("gh_host", "redirect to %s refused: not GitHub over https", got ? h : "?"); break; }
                    continue;
                }
                if (st == 403 || st == 429)
                    fail("gh_rate", "GitHub said %d: 60 checks an hour per address", st);
                else if (st == 404)
                    fail("gh_none", "GitHub said 404: no published release yet");
                else
                    { char code[24]; snprintf(code, sizeof(code), "gh_http %d", st); fail(code); }
                break;
            }
            if (hop == HOPS) fail("gh_redirects");
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            return nullptr;
        }

        void done(esp_http_client_handle_t c)
        {
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
        }

        // ArduinoJson's custom reader over the open response.
        struct BodyReader {
            esp_http_client_handle_t c;
            char buf[256];
            int  n = 0, i = 0;
            int read()
            {
                if (i >= n) {
                    n = esp_http_client_read(c, buf, sizeof(buf));
                    i = 0;
                    if (n <= 0) { n = 0; return -1; }
                }
                return (uint8_t)buf[i++];
            }
            size_t readBytes(char* d, size_t k)
            {
                size_t m = 0;
                for (int ch; m < k && (ch = read()) >= 0;) d[m++] = (char)ch;
                return m;
            }
        };

        void runCheck()
        {
            int64_t len = 0;
            esp_http_client_handle_t c = get(LATEST_URL, true, len);
            if (!c) return;
            JsonDocument f(&s_json), doc(&s_json);
            GhRelease::filter(f);
            BodyReader rd{c};
            const DeserializationError je = deserializeJson(doc, rd, DeserializationOption::Filter(f));
            done(c);
            if (je) { fail("gh_json", "%s", je.c_str()); return; }

            GhRelease::Release r;
            const GhRelease::Pick p = GhRelease::pick(doc.as<JsonVariantConst>(), MegaOpen::envName(), r);
            portENTER_CRITICAL(&s_mux);
            snprintf(s_v.latest, sizeof(s_v.latest), "%s", r.tag);
            snprintf(s_v.asset, sizeof(s_v.asset), "%s", r.asset);
            s_v.size = r.size;
            portEXIT_CRITICAL(&s_mux);
            if (p != GhRelease::Pick::OK) { fail(GhRelease::pickText(p)); return; }

            GhRelease::Ver mine;
            const bool newer = !GhRelease::parse(MEGAOPEN_VERSION, mine) ||
                               GhRelease::compare(r.ver, mine) > 0;
            s_rel = r;
            setPhase(newer ? Phase::Available : Phase::Current);
            LogBus::printf("[ota] github: latest %s, this %s: %s", r.tag, MEGAOPEN_VERSION,
                           newer ? "update available" : "up to date");
        }

        bool fetchSum(uint8_t want[32])
        {
            int64_t len = 0;
            esp_http_client_handle_t c = get(s_rel.sumsUrl, false, len);
            if (!c) return false;
            char* b = (char*)psMalloc(SUMS_MAX);
            if (!b) { done(c); fail("no_memory", "for SHA256SUMS"); return false; }
            size_t n = 0;
            for (int k; n < SUMS_MAX && (k = esp_http_client_read(c, b + n, SUMS_MAX - n)) > 0;) n += k;
            done(c);
            const bool ok = GhRelease::sumFor(b, n, s_rel.asset, want);
            heap_caps_free(b);
            if (!ok) fail("gh_no_sum_line", "SHA256SUMS has no line for %s", s_rel.asset);
            return ok;
        }

        void runInstall()
        {
            uint8_t want[32];
            if (!fetchSum(want)) return;

            int64_t len = 0;
            esp_http_client_handle_t c = get(s_rel.binUrl, false, len);
            if (!c) return;
            if (len <= 0 || (s_rel.size && (uint64_t)len != s_rel.size)) {
                done(c);
                fail("gh_size", "the image is %lld bytes, the release says %u", (long long)len, (unsigned)s_rel.size);
                return;
            }
            // esp_ota_* and not Arduino's Update (which the page's upload
            // keeps): no 4 kB internal sector buffer. Sequential writes erase
            // one sector as the image reaches it, as Update does, never the
            // whole slot up front with the cache off for seconds.
            const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
            if (!part || (uint64_t)len > part->size) {
                done(c);
                fail("gh_slot", part ? "the image does not fit the OTA slot" : "no OTA slot");
                return;
            }
            uint8_t* buf = (uint8_t*)heap_caps_malloc(CHUNK, CAP_IN);
            if (!buf) { done(c); fail("no_memory", "for the download"); return; }
            esp_ota_handle_t ota = 0;
            const esp_err_t be = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
            if (be != ESP_OK) {
                heap_caps_free(buf);
                done(c);
                fail("gh_slot", "cannot open the OTA slot (%s)", esp_err_to_name(be));
                return;
            }
            portENTER_CRITICAL(&s_mux);
            s_v.phase = Phase::Downloading;
            s_v.size  = (uint32_t)len;
            s_v.got   = 0;
            portEXIT_CRITICAL(&s_mux);
            LogBus::printf("[ota] github: downloading %s (%u bytes)", s_rel.asset, (unsigned)len);

            mbedtls_md_context_t md;
            mbedtls_md_init(&md);
            mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
            mbedtls_md_starts(&md);

            uint32_t got = 0;
            const char* broke = nullptr;
            // Per tenth of the image: the lowest internal heap seen, ours and
            // the allocator's own low-water mark, and its duration.
            constexpr uint32_t CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
            uint8_t  step = 0;
            uint32_t stepLow = heapNow(), stepBig = 0, stepAt = millis();
            uint32_t sysLow = heap_caps_get_minimum_free_size(CAPS);
            portENTER_CRITICAL(&s_mux);
            memset(s_v.stepMin, 0, sizeof(s_v.stepMin));
            memset(s_v.stepMs, 0, sizeof(s_v.stepMs));
            portEXIT_CRITICAL(&s_mux);
            while (got < (uint32_t)len) {
                const int k = esp_http_client_read(c, (char*)buf, CHUNK);
                if (k <= 0) { broke = "the download broke off"; break; }
                mbedtls_md_update(&md, buf, (size_t)k);
                const esp_err_t we = esp_ota_write(ota, buf, (size_t)k);
                if (we != ESP_OK) { broke = esp_err_to_name(we); break; }
                got += (uint32_t)k;
                sampleHeap();
                const uint32_t h = heapNow();
                if (h < stepLow) { stepLow = h; stepBig = heap_caps_get_largest_free_block(CAPS); }
                portENTER_CRITICAL(&s_mux);
                s_v.got = got;
                portEXIT_CRITICAL(&s_mux);
                const uint8_t tenth = (uint8_t)((uint64_t)got * 10 / (uint64_t)len);   // 0..10
                while (step < tenth && step < 10) {
                    // A low-water mark that moved during this tenth was a dip
                    // this task did not see (it was waiting for the network).
                    const uint32_t sys = heap_caps_get_minimum_free_size(CAPS);
                    const uint32_t low = sys < sysLow ? sys : stepLow;
                    const uint32_t now = millis();
                    portENTER_CRITICAL(&s_mux);
                    s_v.stepMin[step] = low;
                    s_v.stepMs[step]  = now - stepAt;
                    if (low < s_v.heapMin) s_v.heapMin = low;
                    portEXIT_CRITICAL(&s_mux);
                    LogBus::printf("[ota] github: %u%%, internal heap now %u, low %u%s (largest block %u), %u ms",
                                   (unsigned)(step + 1) * 10, (unsigned)h, (unsigned)low,
                                   sys < sysLow ? " system" : "", (unsigned)stepBig,
                                   (unsigned)(now - stepAt));
                    sysLow  = sys;
                    stepLow = h;
                    stepBig = 0;
                    stepAt  = now;
                    ++step;
                }
            }
            uint8_t have[32];
            mbedtls_md_finish(&md, have);
            mbedtls_md_free(&md);
            heap_caps_free(buf);
            done(c);

            if (broke) {
                esp_ota_abort(ota);
                char code[40];
                snprintf(code, sizeof(code), "gh_broke %u %u", (unsigned)got, (unsigned)len);
                fail(code, "%s", broke);
                return;
            }
            if (memcmp(have, want, 32) != 0) {
                esp_ota_abort(ota);
                fail("gh_sha", "SHA-256 does not match SHA256SUMS: nothing was changed");
                return;
            }
            // end() checks the image itself (header, chip, its own hash).
            const esp_err_t ee = esp_ota_end(ota);
            if (ee != ESP_OK) { fail("gh_verify", "%s", esp_err_to_name(ee)); return; }
            const esp_err_t se = esp_ota_set_boot_partition(part);
            if (se != ESP_OK) { fail("gh_boot", "cannot set the boot partition (%s)", esp_err_to_name(se)); return; }

            // The done line never comes after a good install: the stack's
            // headroom is reported here, at the deepest point it will reach.
            const uint32_t stackFree = uxTaskGetStackHighWaterMark(nullptr);
            s_rebootAsked = false;
            portENTER_CRITICAL(&s_mux);
            s_v.phase     = Phase::Written;
            s_v.stackFree = stackFree;
            portEXIT_CRITICAL(&s_mux);
            LogBus::printf("[ota] github: %s written, SHA-256 matches; internal heap min %u of %u; "
                           "stack %u of %u unused; reboot when parked or from the page",
                           s_rel.tag, (unsigned)s_v.heapMin, (unsigned)s_v.heapStart,
                           (unsigned)stackFree, (unsigned)App::Plan::GHOTA_STACK);
            // Into the flash log now, not at the reboot: the car turned off
            // while waiting boots the new image with no shutdown handler run.
            LogBus::flushAndWait(500);
        }

        // After a good install, with mbedTLS back on its own allocator: never a
        // reboot mid-drive. The page's confirm (refused while moving), or the
        // car standing with a fresh speed of 0.
        [[noreturn]] void waitAndReboot()
        {
            const char* why = nullptr;
            while (!why) {
                if (s_rebootAsked)  why = "from the page";
                else if (parked())  why = "the car is standing";
                else vTaskDelay(pdMS_TO_TICKS(WAIT_POLL_MS));
            }
            setPhase(Phase::Rebooting);
            LogBus::printf("[ota] github: rebooting into the trial (%s)", why);
            vTaskDelay(pdMS_TO_TICKS(PAGE_SEES_REBOOT_MS));   // the page's next poll shows it
            LogBus::flushAndWait(300);
            ESP.restart();
            for (;;) vTaskDelay(pdMS_TO_TICKS(1000));   // restart() does not return
        }

        void task(void* arg)
        {
            const Job job = (Job)(uintptr_t)arg;
            stage(&View::heapTask);
            {
                TlsJob::InPsram tls;
                portENTER_CRITICAL(&s_mux);
                s_v.tlsInPsram = tls.on;
                portEXIT_CRITICAL(&s_mux);
                if (job == Job::Check) runCheck();
                else                   runInstall();
            }
            TlsJob::release();   // mbedTLS is back on its own allocator
            View v;
            view(v);
            if (v.phase == Phase::Written) waitAndReboot();
            // ESP-IDF counts the high-water mark in bytes.
            const uint32_t stackFree = uxTaskGetStackHighWaterMark(nullptr);
            LogBus::printf("[ota] github: %s done; internal heap %u, task created %u, running %u, "
                           "client %u, after TLS %u, min %u (TLS in %s); stack %u of %u unused",
                           job == Job::Check ? "check" : "install", (unsigned)s_v.heapStart,
                           (unsigned)s_v.heapCreated, (unsigned)s_v.heapTask,
                           (unsigned)s_v.heapClient, (unsigned)s_v.heapTls,
                           (unsigned)s_v.heapMin, s_v.tlsInPsram ? "PSRAM" : "internal RAM",
                           (unsigned)stackFree, (unsigned)App::Plan::GHOTA_STACK);
            portENTER_CRITICAL(&s_mux);
            s_v.stackFree = stackFree;
            s_busy = false;
            portEXIT_CRITICAL(&s_mux);
            vTaskDelete(nullptr);
        }

        const char* start(Job job)
        {
            portENTER_CRITICAL(&s_mux);
            if (s_busy) {
                const bool written = s_v.phase == Phase::Written;
                portEXIT_CRITICAL(&s_mux);
                return written ? "fw_written" : "fw_running";
            }
            if (job == Job::Install && s_v.phase != Phase::Available) {
                portEXIT_CRITICAL(&s_mux);
                return "fw_no_newer";   // check first
            }
            // The log push may be mid-upload: a few seconds at most.
            if (!TlsJob::claim("ghota")) {
                portEXIT_CRITICAL(&s_mux);
                return "tls_busy";
            }
            s_busy       = true;
            s_v.phase    = job == Job::Check ? Phase::Checking : Phase::Downloading;
            s_v.err[0]   = 0;
            s_v.got      = 0;
            s_v.heapStart = s_v.heapMin = heapNow();
            s_v.heapCreated = s_v.heapTask = s_v.heapClient = s_v.heapTls = 0;
            portEXIT_CRITICAL(&s_mux);

            if (xTaskCreatePinnedToCore(task, "ghota", App::Plan::GHOTA_STACK, (void*)(uintptr_t)job,
                                        App::Plan::GHOTA_PRIO, nullptr,
                                        App::core(App::Plan::GHOTA_CORE)) != pdPASS) {
                TlsJob::release();
                portENTER_CRITICAL(&s_mux);
                s_busy    = false;
                s_v.phase = Phase::Failed;
                snprintf(s_v.err, sizeof(s_v.err), "no_memory");
                portEXIT_CRITICAL(&s_mux);
                return "no_memory";
            }
            // Right after the create, still on httpd: the stack and TCB alone.
            // The task's own first sample comes whenever priority 1 gets the
            // core, after whatever httpd and WiFi did meanwhile.
            stage(&View::heapCreated);
            return nullptr;
        }
    }

    const char* phaseId(Phase p)
    {
        switch (p) {
            case Phase::Idle:        return "idle";
            case Phase::Checking:    return "checking";
            case Phase::Current:     return "current";
            case Phase::Available:   return "available";
            case Phase::Downloading: return "downloading";
            case Phase::Written:     return "written";
            case Phase::Rebooting:   return "rebooting";
            case Phase::Failed:      return "failed";
        }
        return "?";
    }

    void view(View& out)
    {
        portENTER_CRITICAL(&s_mux);
        out = s_v;
        portEXIT_CRITICAL(&s_mux);
    }

    const char* check() { return start(Job::Check); }

    const char* install()
    {
        if (moving()) return "fw_moving";
        if (Update.isRunning()) {
            // A /ota/start with no upload after it holds Update open for ever.
            if (s_local && (uint32_t)(millis() - s_localAt) < LOCAL_STALE_MS)
                return "fw_upload";   // an upload from the page is running
            Update.abort();
            s_local = false;
            LogBus::line("[ota] github: cleared a local upload session left open");
        }
        return start(Job::Install);
    }

    const char* reboot()
    {
        View v;
        view(v);
        if (v.phase != Phase::Written) return "fw_nothing";
        if (moving()) return "fw_moving";
        s_rebootAsked = true;
        return nullptr;
    }

    void attachCar(const Vehicle::State* car) { s_car = car; }

    bool moving()
    {
        const uint32_t now = millis();
        return s_car && s_car->fresh(Vehicle::SIG_SPEED, now, SPEED_FRESH_MS) &&
               s_car->get(Vehicle::SIG_SPEED) > MOVING_KMH_X100;
    }

    bool busy()
    {
        portENTER_CRITICAL(&s_mux);
        const bool b = s_busy;
        portEXIT_CRITICAL(&s_mux);
        return b;
    }

    void localStart() { s_localAt = millis(); s_local = true; }
    void localChunk() { s_localAt = millis(); }
    void localEnd()   { s_local = false; }
}
#endif
