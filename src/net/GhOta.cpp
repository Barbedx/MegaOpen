#include "GhOta.h"
#if FEATURE_GH_OTA
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <mbedtls/md.h>
#include <mbedtls/platform.h>
#include "GhRelease.h"
#include "GhRoots.h"
#include "../Version.h"
#include "../app/TaskPlan.h"
#include "../utils/LogBus.h"

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
        constexpr size_t   CHUNK      = 4096;
        constexpr size_t   SUMS_MAX   = 4096;    // three envs x two images: ~600 bytes
        constexpr uint32_t PAGE_SEES_REBOOT_MS = 1500;

        portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
        View s_v;                              // under s_mux
        bool s_busy = false;                   // under s_mux: a job task exists
        GhRelease::Release s_rel;              // the last check's pick; the job owns it

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

        // Failed, with the reason on the page and in the log.
        void fail(const char* fmt, ...)
        {
            char b[sizeof(s_v.err)];
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(b, sizeof(b), fmt, ap);
            va_end(ap);
            portENTER_CRITICAL(&s_mux);
            s_v.phase = Phase::Failed;
            memcpy(s_v.err, b, sizeof(b));
            portEXIT_CRITICAL(&s_mux);
            LogBus::printf("[ota] github: %s", b);
        }

        // mbedTLS into PSRAM for the job: the prebuilt sdkconfig pins it to
        // internal RAM (MBEDTLS_INTERNAL_MEM_ALLOC), and a handshake's ~40 kB
        // there is most of what is free with BLE up. Internal RAM when PSRAM
        // is full. heap_caps_free frees either, so the swap back is safe with
        // blocks still out.
        void* psramCalloc(size_t n, size_t size)
        {
            return heap_caps_calloc_prefer(n, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        // What IDF's own allocator does under MBEDTLS_INTERNAL_MEM_ALLOC; its
        // header declares it without extern "C", so C++ cannot link to it.
        void* internalCalloc(size_t n, size_t size)
        {
            return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }

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

        struct TlsInPsram {
            bool on = false;
            TlsInPsram()
            {
#if CONFIG_SPIRAM
                on = psramFound();
                if (on) mbedtls_platform_set_calloc_free(psramCalloc, heap_caps_free);
#endif
            }
            ~TlsInPsram()
            {
                if (on) mbedtls_platform_set_calloc_free(internalCalloc, heap_caps_free);
            }
        };

        // A GET that follows redirects (a release asset is a 302 to the CDN,
        // with a ~1 kB Location: hence the buffers). The open client, headers
        // read, status 200; nullptr after fail().
        esp_http_client_handle_t get(const char* url, bool api, int64_t& len)
        {
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
            if (!c) { fail("no memory for the HTTP client"); return nullptr; }
            stage(&View::heapClient);
            if (api) esp_http_client_set_header(c, "Accept", "application/vnd.github+json");

            int hop = 0;
            for (; hop < HOPS; ++hop) {
                const esp_err_t e = esp_http_client_open(c, 0);
                if (e != ESP_OK) {
                    fail("cannot connect (%s): no internet, or TLS", esp_err_to_name(e));
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
                    continue;
                }
                if (st == 403 || st == 429)
                    fail("GitHub said %d: the API allows 60 checks an hour per address, try later", st);
                else if (st == 404)
                    fail("GitHub said 404: no published release yet");
                else
                    fail("GitHub said %d", st);
                break;
            }
            if (hop == HOPS) fail("too many redirects");
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
            if (je) { fail("the release JSON did not parse (%s)", je.c_str()); return; }

            GhRelease::Release r;
            const GhRelease::Pick p = GhRelease::pick(doc.as<JsonVariantConst>(), MegaOpen::envName(), r);
            portENTER_CRITICAL(&s_mux);
            snprintf(s_v.latest, sizeof(s_v.latest), "%s", r.tag);
            snprintf(s_v.asset, sizeof(s_v.asset), "%s", r.asset);
            s_v.size = r.size;
            portEXIT_CRITICAL(&s_mux);
            if (p != GhRelease::Pick::OK) { fail("%s", GhRelease::pickText(p)); return; }

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
            if (!b) { done(c); fail("no memory for SHA256SUMS"); return false; }
            size_t n = 0;
            for (int k; n < SUMS_MAX && (k = esp_http_client_read(c, b + n, SUMS_MAX - n)) > 0;) n += k;
            done(c);
            const bool ok = GhRelease::sumFor(b, n, s_rel.asset, want);
            heap_caps_free(b);
            if (!ok) fail("SHA256SUMS has no line for %s", s_rel.asset);
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
                fail("the image is %lld bytes, the release says %u", (long long)len, (unsigned)s_rel.size);
                return;
            }
            // PSRAM is fine: Update copies it into its own sector buffer before
            // the flash write.
            uint8_t* buf = (uint8_t*)psMalloc(CHUNK);
            if (!buf) { done(c); fail("no memory for the download"); return; }
            if (!Update.begin((size_t)len)) {
                heap_caps_free(buf);
                done(c);
                fail("no OTA slot: %s", Update.errorString());
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

            uint32_t got = 0, lastPct = 0;
            const char* why = nullptr;
            while (got < (uint32_t)len) {
                const int k = esp_http_client_read(c, (char*)buf, CHUNK);
                if (k <= 0) { why = "the download broke off"; break; }
                mbedtls_md_update(&md, buf, (size_t)k);
                if (Update.write(buf, (size_t)k) != (size_t)k) { why = Update.errorString(); break; }
                got += (uint32_t)k;
                sampleHeap();
                portENTER_CRITICAL(&s_mux);
                s_v.got = got;
                portEXIT_CRITICAL(&s_mux);
                const uint32_t pct = (uint32_t)((uint64_t)got * 100 / (uint64_t)len);
                if (pct >= lastPct + 25) {
                    lastPct = pct - pct % 25;
                    LogBus::printf("[ota] github: %u%%, internal heap %u (min %u)", (unsigned)pct,
                                   (unsigned)heapNow(), (unsigned)s_v.heapMin);
                }
            }
            uint8_t have[32];
            mbedtls_md_finish(&md, have);
            mbedtls_md_free(&md);
            heap_caps_free(buf);
            done(c);

            if (why) { Update.abort(); fail("%s at %u of %u bytes", why, (unsigned)got, (unsigned)len); return; }
            if (memcmp(have, want, 32) != 0) {
                Update.abort();
                fail("SHA-256 does not match SHA256SUMS: nothing was changed");
                return;
            }
            if (!Update.end()) { fail("the image did not verify: %s", Update.errorString()); return; }

            // The done line never comes after a good install: the stack's
            // headroom is reported here, at the deepest point it will reach.
            const uint32_t stackFree = uxTaskGetStackHighWaterMark(nullptr);
            portENTER_CRITICAL(&s_mux);
            s_v.phase     = Phase::Rebooting;
            s_v.stackFree = stackFree;
            portEXIT_CRITICAL(&s_mux);
            LogBus::printf("[ota] github: %s written, SHA-256 matches; internal heap min %u of %u; "
                           "stack %u of %u unused; rebooting into the trial",
                           s_rel.tag, (unsigned)s_v.heapMin, (unsigned)s_v.heapStart,
                           (unsigned)stackFree, (unsigned)App::Plan::GHOTA_STACK);
            vTaskDelay(pdMS_TO_TICKS(PAGE_SEES_REBOOT_MS));   // the page's next poll shows it
            LogBus::flushAndWait(300);
            ESP.restart();
        }

        void task(void* arg)
        {
            const Job job = (Job)(uintptr_t)arg;
            stage(&View::heapTask);
            {
                TlsInPsram tls;
                portENTER_CRITICAL(&s_mux);
                s_v.tlsInPsram = tls.on;
                portEXIT_CRITICAL(&s_mux);
                if (job == Job::Check) runCheck();
                else                   runInstall();
            }
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
            if (s_busy) { portEXIT_CRITICAL(&s_mux); return "a check or an update is already running"; }
            if (job == Job::Install && s_v.phase != Phase::Available) {
                portEXIT_CRITICAL(&s_mux);
                return "no newer release: check first";
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
                portENTER_CRITICAL(&s_mux);
                s_busy    = false;
                s_v.phase = Phase::Failed;
                snprintf(s_v.err, sizeof(s_v.err), "no memory for the task");
                portEXIT_CRITICAL(&s_mux);
                return "no memory for the task";
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
        if (Update.isRunning()) return "an upload from the page is running";
        return start(Job::Install);
    }
}
#endif
