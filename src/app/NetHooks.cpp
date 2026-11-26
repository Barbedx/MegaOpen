#include "NetHooks.h"
#include <WiFi.h>          // the heartbeat reports SSID/BSSID/gateway directly
#include <ArduinoJson.h>
#include "Housekeeping.h"
#include "CarTask.h"
#include "HuLink.h"
#include "KeyPath.h"
#include "LoopTask.h"
#include "OtaTrial.h"
#include "../Version.h"
#include "Supervisor.h"
#include "../link/AppLink.h"
#include "../utils/AllocWatch.h"
#include "../utils/LogQ.h"
#if FEATURE_IMPROV
#include "../net/ImprovSerial.h"
#endif
#if FEATURE_GH_OTA || FEATURE_LOG_PUSH
#include "../net/NetJob.h"
#endif
#if FEATURE_BLE
#include "../ble/BleHub.h"
#endif
#include "lwip/sockets.h"
#include "../net/IpNotice.h"
#include "../net/LogPush.h"
#include "../net/WebUi.h"
#include "../net/WifiManager.h"
#include "../sinks/UartTx.h"
#include "../utils/Log.h"
#include "../utils/PsramJson.h"
#if FEATURE_DISPLAY_CAN || FEATURE_BLE
#include "Media.h"
#endif
#if FEATURE_DISPLAY_CAN
#include "PanelMenu.h"
#include "UiTask.h"
#endif
#if FEATURE_BLE
#include "../ble/BleService.h"
#endif
#if FEATURE_KNOBS
#include "../knobs/KnobService.h"
#endif
#if FEATURE_CAN2
#include "../bus/Mcp2518Source.h"
#endif

namespace App
{
    namespace NetHooks
    {
        namespace
        {
            Cfg::Config* s_cfg  = nullptr;
            WebUi*       s_web  = nullptr;
            WifiManager* s_wifi = nullptr;
#if FEATURE_KNOBS
            const Knobs::Service* s_knobs = nullptr;
#endif

            // Task handles for the [stk] stack high-water line, which runs on net_svc
            // and so must name the loop's own handle rather than ask for "this task".
            TaskHandle_t bleTask()
            {
#if FEATURE_BLE
                return BleService::task();
#else
                return nullptr;
#endif
            }

            // ---------------------------------------------------------------------------
            // Web servicing (ElegantOTA), on net_svc (src/app/Housekeeping.h)
            // ---------------------------------------------------------------------------
            // Not BLE-specific: must stay outside any FEATURE_BLE gate.
            // The loop task owns the panel's deadlines, so the web's periodic work runs on
            // net_svc instead: priority 1, on core 0, never in front of the panel or a key.
            static void hkWeb(uint32_t now)
            {
                (void)now;
                s_web->loop();
#if FEATURE_DISPLAY_CAN
                // Values chosen in the panel menu: stored here, with every other setting.
                App::PanelMenu::applyPending();
#endif
            }

            // ---- net_svc's hooks (src/app/Housekeeping.h) ---------------------------------
            // WiFi usable at least once since boot: the next OTA needs it. Our own AP
            // counts: the next OTA arrives through it just as well. Counting STA only
            // meant an image flashed in the car (AP, no known network) was rolled back
            // and rebooted 180 s after every OTA.
            // The OTA trial wants a way in for the next update: WiFi up once, or the app
            // on the link (USB or BLE) (it carries the page and the upload too).
            // The web server's port takes a TCP connection from the board itself
            // (the loopback the app link uses). Sticky; tried once a second until
            // it does. It says "the server listens", not "every route works".
            static bool hkWebUp()
            {
                static bool s_ok = false;
                static uint32_t s_at = 0;
                if (s_ok) return true;
                const uint32_t now = millis();
                if (s_at && (uint32_t)(now - s_at) < 1000) return false;
                s_at = now ? now : 1;
                if (!xTaskGetHandle("httpd")) return false;
                const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
                if (fd < 0) return false;
                struct timeval tv = {0, 300000};
                ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                struct sockaddr_in a;
                memset(&a, 0, sizeof(a));
                a.sin_family = AF_INET;
                a.sin_port = htons(80);
                a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                s_ok = ::connect(fd, (struct sockaddr*)&a, sizeof(a)) == 0;
                ::close(fd);
                return s_ok;
            }

            static bool hkWifiSeen()
            {
                static bool s_seen = false;
                s_seen = s_seen || WiFi.status() == WL_CONNECTED ||
                         (!s_wifi->connecting() && !s_wifi->isSta() && (WiFi.getMode() & WIFI_AP)) ||
                         AppLink::alive();
                return s_seen;
            }

            // WiFi, then the board's address on the panel once per connection
            // (src/net/IpNotice.h): "IP 192.168.1.15 / megaopen.local boot 204" on the
            // home network, "AP MegaOpen-XXXX / 192.168.4.1 boot 204" on our own. Polite:
            // it waits out the menu and a glass hold (App::Ui::queueNotice).
#if FEATURE_DISPLAY_CAN
            // The address as the panel shows it: "IP 192.168.1.15" over the mDNS name
            // (empty with mDNS off) and the boot number, or "AP MegaOpen-XXXX" over
            // 192.168.4.1 and the boot number. The connect notice (net_svc) and the
            // panel menu's address row (ui task; the row shows the head alone).
            void addressTextImpl(char* head, size_t hn, char* body, size_t bn)
            {
                body[0] = 0;
                if (s_wifi->connecting()) {
                    snprintf(head, hn, "WiFi joining");
                } else if (s_wifi->isSta() && WiFi.status() == WL_CONNECTED) {
                    snprintf(head, hn, "IP %s", WiFi.localIP().toString().c_str());
                    snprintf(body, bn, "%s", s_wifi->mdnsHost().c_str());
                } else if (WiFi.getMode() & WIFI_AP) {
                    snprintf(head, hn, "AP %s", s_wifi->apSsid().c_str());
                    snprintf(body, bn, "%s", WiFi.softAPIP().toString().c_str());
                } else {
                    snprintf(head, hn, "WiFi down");
                }
                // The boot number after the name or address ("megaopen.local boot 204"):
                // read off the panel for a report, it names the pushed log's folder
                // (boot-00204). Its own row when the first is full (wrapTwoEllipsis).
                const size_t n = strlen(body);
                snprintf(body + n, bn - n, "%sboot %lu", n ? " " : "", (unsigned long)MegaOpen::bootNo());
            }
#endif
#if FEATURE_LOG_PUSH
            // The status snapshot LogPush sends to GitHub every LogPush::SNAP_MS:
            // /api/status and /api/health in one object, for tools rather than regexes
            // (docs/design/logging.md §5). On the push task; the pools in PSRAM, beside
            // its TLS handshake.
            size_t logPushSnapshotImpl(char* out, size_t cap)
            {
                // dispatch() holds the settings lock around every route; the builders
                // read Strings in Settings (bleMode), so this takes it too (recursive).
                Cfg::Config::Guard g(*s_cfg);
                JsonDocument d(&PsramJson::instance());
                d["uptimeMs"] = millis();
                s_web->statusJson(d["status"].to<JsonObject>());
                s_web->healthJson(d["health"].to<JsonObject>());
                s_web->carJson(d["car"].to<JsonObject>());
                const size_t n = serializeJson(d, out, cap);
                return n < cap ? n : 0;
            }
#endif

            static void hkWifi(uint32_t now)
            {
                s_wifi->loop(now);
                AllocWatch::report(now);   // the failed-alloc hook's line, here on a task (R1)
#if FEATURE_IMPROV
                ImprovSerial::tick(now);   // right after: it calls WifiManager on this task
#endif
#if FEATURE_LOG_PUSH
                // Online: on WiFi with internet, or an app on the USB link that offers its own.
                // Not on an OTA trial: a push takes internal heap down to a few kB, under
                // the trial's healthy line (OtaTrial::HEALTH_HEAP_MIN), and each dip starts
                // its 60 s again, so a good image rolled back. The lines wait in the spool
                // and the flash, and go once the trial has confirmed.
                const bool trial = App::OtaTrial::onTrial();
                static bool s_trialSaid = false;
                if (trial && !s_trialSaid) { s_trialSaid = true; LOGI("push", "held until the OTA trial confirms"); }
                LogPush::tick(now, !trial && ((s_wifi->isSta() && !s_wifi->connecting() && WiFi.status() == WL_CONNECTED)
                                              || AppLink::netAvailable()));
#endif
#if FEATURE_DISPLAY_CAN
                static IpNotice s_notice;
                IpNotice::Mode m = IpNotice::Mode::None;
                uint32_t ip = 0;
                if (s_wifi->connecting()) {
                    // a join or a scan: neither link is settled
                } else if (s_wifi->isSta()) {
                    if (WiFi.status() == WL_CONNECTED) { m = IpNotice::Mode::Sta; ip = (uint32_t)WiFi.localIP(); }
                } else if (WiFi.getMode() & WIFI_AP) {
                    m = IpNotice::Mode::Ap;
                    ip = (uint32_t)WiFi.softAPIP();
                }
                if (!s_notice.tick(now, m, ip)) return;
                char head[40], body[40];
                addressTextImpl(head, sizeof(head), body, sizeof(body));
                App::Ui::queueNotice(head, body);
                LOGI("wifi", "on the panel: %s / %s\n", head, body);
#endif
            }

            // httpd: the web server's own task, where every route runs; found by name
            // once it exists (the server starts after hkBeat's first pass). Never deleted.
            static TaskHandle_t s_httpdTask = nullptr;

            // [win] ctx= at each new heap minimum (App::Housekeeping::Hooks::ctx): plain
            // reads, no lock, no lookup. httpd idles in select(), blocked or suspended:
            // ready or running is a request being served (one held in a send() or a
            // Request/Reply wait reads as idle).
            static uint32_t hkCtx()
            {
                using namespace App::Housekeeping;
                uint32_t c = 0;
#if FEATURE_GH_OTA || FEATURE_LOG_PUSH
                if (NetJob::running()) c |= CTX_JOB;
#endif
                const TaskHandle_t h = s_httpdTask;
                if (h) {
                    const eTaskState st = eTaskGetState(h);
                    if (st == eRunning || st == eReady) c |= CTX_HTTP;
                }
#if FEATURE_BLE
                c |= ctxBle(BleHub::connectedCount());
#endif
                if (s_wifi->connecting()) c |= CTX_JOIN;
                return c;
            }

            // Every 10 s, after [win] and [task]: the lines nobody else owns.
            static void hkBeat(uint32_t now)
            {
                const uint32_t s = now / 1000;
                // Settings › Reminders, live: two bytes, read without the settings lock.
                App::Car::setReminderSpeeds(s_cfg->s().remindLightsKmh, s_cfg->s().remindBrakeKmh);
                App::Car::setGears(s_cfg->s().gearRatio);
                App::Car::store();   // the odometer, the service edge: NVS from this task
                // [stk] is a subset of [task] now. Kept until Step 6 retires it, so old
                // logs and new ones still compare line for line.
                const TaskHandle_t net = App::Housekeeping::task();
                if (!s_httpdTask) s_httpdTask = xTaskGetHandle("httpd");
                const TaskHandle_t httpd = s_httpdTask;
                const unsigned httpdFree = httpd ? (unsigned)uxTaskGetStackHighWaterMark(httpd) : 0u;
                if (httpd) App::Housekeeping::stackGuard("httpd", httpdFree);
                // Then the core's own tasks, whose stacks we do not size (S6).
                char core[160];
                App::Housekeeping::coreStacks(core, sizeof(core));
                LOGS("stk", "loop=%u net=%u ble=%u knobs=%u httpd=%u%s",
                     App::Loop::task() ? (unsigned)uxTaskGetStackHighWaterMark(App::Loop::task()) : 0u,
                     net ? (unsigned)uxTaskGetStackHighWaterMark(net) : 0u,
                     bleTask() ? (unsigned)uxTaskGetStackHighWaterMark(bleTask()) : 0u,
#if FEATURE_KNOBS
                     (unsigned)(s_knobs ? s_knobs->stackFree() : 0u),
#else
                     0u,
#endif
                     httpdFree, core);
                // [hu] and [key] are logged by the hu and keys tasks themselves, [car]
                // by the car task, and [can], [panel], [stall] and [loop] by the loop,
                // each from its own window.
#if FEATURE_CAN2
                // CAN A, from the car task's snapshot: no SPI from here. A silent car bus,
                // a deaf chip and a wrong rate look the same without these.
                // ab = searching | locked | manual at bps; fps = the last second.
                {
                    const Mcp2518Source::Status c = Mcp2518Source::instance().status().read();
                    static const char* const kAb[] = {"searching", "locked", "manual"};
                    LOGS("can2", "live=%u ab=%s bps=%lu fps=%lu frames=%lu hwOvf=%lu ringPeak=%lu/256 "
                         "fd=%lu rtr=%lu intKicks=%lu maskMaxUs=%lu rec=%lu tec=%lu flags=0x%02lX "
                         "bdiag1=0x%08lX restarts=%lu rounds=%lu rejected=%lu err=0x%lX",
                         (unsigned)c.live, kAb[c.state <= 2 ? c.state : 0], (unsigned long)c.bps,
                         (unsigned long)c.fps, (unsigned long)c.frames, (unsigned long)c.hwOverflows,
                         (unsigned long)c.ringPeak, (unsigned long)c.fdDropped,
                         (unsigned long)c.remoteDropped, (unsigned long)c.intKicks,
                         (unsigned long)c.maskMaxUs, (unsigned long)(c.trec & 0xFF),
                         (unsigned long)((c.trec >> 8) & 0xFF), (unsigned long)((c.trec >> 16) & 0x3F),
                         (unsigned long)c.bdiag1, (unsigned long)c.restarts, (unsigned long)c.cycles,
                         (unsigned long)c.rejected, (unsigned long)c.beginErr);
                    // A newly detected rate goes to NVS here, on net_svc, once: the car
                    // task never waits for a flash write, and the next boot tries it first.
                    if (c.detected && c.detected != s_cfg->s().can2Detected) {
                        s_cfg->saveCan2Detected(c.detected);
                        LOGI("can2", "%lu saved: tried first at the next boot", (unsigned long)c.detected);
                    }
                }
#endif
#if FEATURE_BLE
                // The BLE key path since boot: q = the keys-to-ble_svc queue's drops;
                // ams = AMS writes that went out, fail = ones the stack refused;
                // stale = waited over 300 ms; coal = volume presses folded into one
                // pending count; nolink = owed with no phone connected; vol = AMS
                // commands a volume press sends (measured, sinks/AmsVolumeStep.h).
                LOGS("ble", "keys q=%u ams=%u fail=%u hid=%u stale=%u coal=%u nolink=%u vol=%u",
                     (unsigned)Sinks::BleKeyQueue::dropped(), (unsigned)BleService::keys().amsWrites(),
                     (unsigned)BleService::keys().amsFailed(),
                     (unsigned)BleService::keys().hidPresses(), (unsigned)BleService::keys().stale(),
                     (unsigned)BleService::keys().coalesced(), (unsigned)BleService::keys().noLink(),
                     (unsigned)BleService::keys().volSteps());
#endif
#if FEATURE_DISPLAY_CAN || FEATURE_BLE
                // lockMiss = head-unit frames that found a reader holding the media
                // mutex and were applied a pass later; lost = of those, stash overflow.
                LOGS("media", "hu frames=%u text=%u lockMiss=%u lost=%u",
                     (unsigned)App::Media::hu().frames(), (unsigned)(App::HuLink::decoder() ? App::HuLink::decoder()->stats().textFrames : 0),
                     (unsigned)App::Media::hu().lockMisses(), (unsigned)App::Media::hu().stashDropped());
#endif
                // logq drops = LOGQ records (foreign contexts, utils/LogQ.h) lost to a
                // full queue since boot.
                LOGS("hb", "up=%02u:%02u:%02u heap=%u min=%u big=%u canrx=%u keys=%u tx=%u/%u logq drops=%u",
                     (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60),
                     (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                     (unsigned)ESP.getMaxAllocHeap(),
#if FEATURE_DISPLAY_CAN || FEATURE_VEHICLE_CAN
                     (unsigned)*App::Loop::canRx(),
#else
                     0u,
#endif
                     (unsigned)App::KeyPath::engine().events(),
                     (unsigned)App::HuLink::tx().keyFrames(), (unsigned)App::HuLink::tx().vehicleFrames(),
                     (unsigned)LogQ::dropped());
            }

            // Once a minute: address, SSID, BSSID and gateway, to find the board and
            // confirm which network (and radio) it is really on. Not every 10 s: WiFi.SSID()
            // and the toString() calls allocate, and this was the loop's String work.
            static void hkBeatSlow(uint32_t)
            {
                if (s_wifi->connecting()) {
                    LOGI("net", "joining \"%s\"", s_wifi->curSsid().c_str());
                } else if (s_wifi->isSta()) {
                    // A String whose allocation failed has a NULL buffer; strlen(NULL)
                    // in vsnprintf reboots. Print "?" instead.
                    auto nn = [](const char* s) { return s ? s : "?"; };
                    LOGS("net", "STA ssid=\"%s\" rssi=%d bssid=%s ip=%s gw=%s mask=%s %s",
                         nn(WiFi.SSID().c_str()), (int)WiFi.RSSI(), nn(WiFi.BSSIDstr().c_str()),
                         nn(WiFi.localIP().toString().c_str()),
                         nn(WiFi.gatewayIP().toString().c_str()),
                         nn(WiFi.subnetMask().toString().c_str()),
                         s_wifi->mdnsHost().length() ? nn(s_wifi->mdnsHost().c_str()) : "(no mdns)");
                } else {
                    LOGI("net", "AP \"%s\" ip=%s %s", s_wifi->apSsid().c_str(),
                         s_wifi->ip().c_str(),
                         s_wifi->mdnsHost().length() ? s_wifi->mdnsHost().c_str() : "(no mdns)");
                }
            }
        }

        void bind(Cfg::Config& cfg, WebUi& web, WifiManager& wifi)
        {
            s_cfg  = &cfg;
            s_web  = &web;
            s_wifi = &wifi;
        }

#if FEATURE_KNOBS
        void attachKnobs(const Knobs::Service* knobs) { s_knobs = knobs; }
#endif

        void start()
        {
            // Started after every attach(): it calls into g_web immediately. The web,
            // WiFi, the heartbeat and the OTA verdict: none of them on the
            // panel's task any more (src/app/Housekeeping.h).
            {
                App::Housekeeping::Hooks h;
                h.web      = hkWeb;
                h.wifi     = hkWifi;
                h.wifiSeen = hkWifiSeen;
                h.webUp    = hkWebUp;
                h.beat     = hkBeat;
                h.beatSlow = hkBeatSlow;
                h.ctx      = hkCtx;
                // net_svc checks every critical task's age from its first pass, and
                // this one has been in setup() for seconds without a beat.
                App::Loop::beat();
                App::Housekeeping::start(h);
                // From here a task late past its Plan::LATE_LIMITS is logged.
                App::Supervisor::begin();
            }
        }

#if FEATURE_DISPLAY_CAN
        void addressText(char* head, size_t hn, char* body, size_t bn) { addressTextImpl(head, hn, body, bn); }
#endif
#if FEATURE_LOG_PUSH
        size_t logPushSnapshot(char* out, size_t cap) { return logPushSnapshotImpl(out, cap); }
#endif
    }
}
