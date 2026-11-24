#include "BleService.h"
#if FEATURE_BLE
#include "BleHub.h"
#include "HidRole.h"
#include "../bluetooth.h"
#include "../app/BootLog.h"
#include "../app/KeyPath.h"
#include "../app/Media.h"
#include "../app/Rt.h"
#include "../app/TaskPlan.h"
#include "../app/TaskRegistry.h"
#include "../media/AmsMediaSource.h"
#include "../utils/Log.h"
#if FEATURE_ANCS
#include "../apple_notification_service.h"
#endif
#if FEATURE_DISPLAY_CAN
#include "../app/UiTask.h"
#endif

using App::Boot::memMark;

namespace BleService
{
    namespace
    {
        static AmsMediaSource     g_ams;
        // ble_svc performs what the keys task's BLE proxies post (sinks/BleKeyProxy.h).
        static Sinks::BleKeyWorker g_bleKeys;

        // The live settings (ANCS reads its popup switch per notification).
        const Cfg::Settings* s_settings = nullptr;
        TaskHandle_t g_bleTask = nullptr;
        int          g_bleTaskId = -1;

        // ---------------------------------------------------------------------------
        // BLE service, on its own task
        // ---------------------------------------------------------------------------
        // ServiceAms() and ANCS Process() may block on GATT, and AffaDisplay's deadlines
        // (ACK timeout, 5 s PEER_ALIVE watchdog) are wall clock; from loop() this stalled
        // sync at 0x08 with 401/644 renders Timeout. A blocked task yields; a loop cannot.
        // It also performs the keys' AMS writes and HID presses, which therefore share
        // a task with ServiceAms and can no longer race it over the one client.
#if FEATURE_ANCS
        static void ancsService();
#endif

        static void bleServiceTask(void*)
        {
            bool amsPending = false;
            for (;;) {
                // Woken by a key for the phone or the head unit, or after 20 ms (the
                // old fixed sleep). While AMS steps are owed it does not sleep beyond
                // a tick: each write waits for its response, and that paces it. While
                // a HID release is owed, it wakes when that is due. With no peer
                // connected there is nothing to poll for: BLE_IDLE_MS (S7).
                uint32_t waitMs = amsPending ? 1 : BleHub::connectedCount() ? 20 : App::Plan::BLE_IDLE_MS;
                uint32_t holdLeft = 0;
                if (g_bleKeys.hidHoldLeft(millis(), holdLeft) && holdLeft < waitMs)
                    waitMs = holdLeft ? holdLeft : 1;
                g_bleKeys.collect(waitMs);
                const uint32_t t0 = micros();
                // HID presses went out in collect(). Asked before the release below,
                // so a hold that ends in between still gets its release first.
                const bool hidOwed = g_bleKeys.hidHoldLeft(millis(), holdLeft);
                // The HID release, ahead of any AMS write: Service() does it again
                // first thing; that is a no-op.
                if (BleHub::hidActive()) Hid::service();
                // At most one AMS write, and none while a HID release is still owed:
                // the write holds this task for 30-130 ms, and the release would wait
                // behind it.
                amsPending = hidOwed ? g_bleKeys.pending() : g_bleKeys.step();
                // AMS's long read of a cut title holds this task for two GATT round
                // trips: not while a HID release is owed or key writes wait.
                Bluetooth::SetKeysBusy(g_bleKeys.hidHoldLeft(millis(), holdLeft) || g_bleKeys.pending());
                BleHub::Service();
#if FEATURE_ANCS
                ancsService();
#endif
                // busyMax here is how long GATT setup, ANCS and one AMS write can hold
                // this task.
                App::Tasks::beat(g_bleTaskId, micros() - t0);
            }
        }

#if FEATURE_ANCS
        // Runs on the NimBLE host task (GATT subscription callback): no display calls,
        // nothing that blocks. Rows go to the ui task (App::Ui) under its spinlock.
        static uint32_t g_ancsCount = 0;
        // One notification owed to ble_svc: the callback only copies it here.
        // A newer one replaces an older one not yet shown (the panel has one
        // popup; every notification is still in the library's own [ANCS] line).
        struct AncsOwed { char title[64]; char text[192]; uint8_t cat; bool due; };
        static AncsOwed     s_ancs = {};
        static portMUX_TYPE s_ancsMux = portMUX_INITIALIZER_UNLOCKED;

        static void onAncsNotification(const AppleNotificationService::NotificationInfo& n)
        {
            ++g_ancsCount;
            // The host task's stack is NimBLE's, not ours to size: no LOGI (a
            // 320 B line) and no row folding (~420 B) on it. Two bounded copies.
            portENTER_CRITICAL(&s_ancsMux);
            strlcpy(s_ancs.title, n.title.c_str(), sizeof(s_ancs.title));
            strlcpy(s_ancs.text, n.message.c_str(), sizeof(s_ancs.text));
            s_ancs.cat = (uint8_t)n.categoryId;
            s_ancs.due = true;
            portEXIT_CRITICAL(&s_ancsMux);
        }

        // ble_svc: the line and the popup of the notification the callback left.
        static void ancsService()
        {
            if (!s_ancs.due) return;
            static AncsOwed n;      // 260 B: not on this task's stack
            portENTER_CRITICAL(&s_ancsMux);
            n = s_ancs;
            s_ancs.due = false;
            portEXIT_CRITICAL(&s_ancsMux);
            const char* cat = AppleNotificationService::CategoryName(n.cat);
            // Always logged, in raw UTF-8.
            LOGI("ancs", "%s | %s: %s", cat, n.title, n.text);
#if FEATURE_DISPLAY_CAN
            if (!s_settings->ancsPopup) return;
            App::Ui::queueNotification(n.title[0] ? n.title : cat, n.text);
#endif
        }
#endif
    }

    void begin(const Cfg::Settings& s)
    {
        s_settings = &s;
        // Before any key can be posted, BLE on or not: a proxy with no ble_svc
        // behind it drops and counts.
        if (!Sinks::BleKeyQueue::begin())
            LOGI("ble", "KEY QUEUE FAILED TO ALLOCATE - phone and BLE keyboard keys are dead");
        {
            const auto& bm = s.bleMode;
            // Off = BleHub not started (saves ~40 KB of host state).
            const bool bleOn = (bm == "ams" || bm == "keyboard" || bm == "both" || bm == "app");
            BleHub::Mode mode = BleHub::Mode::Ams;
            if      (bm == "keyboard") mode = BleHub::Mode::Keyboard;
            else if (bm == "both")     mode = BleHub::Mode::Both;
            else if (bm == "app")      mode = BleHub::Mode::App;
            // For /api/health. Not BleHub::amsActive() alone: its gMode defaults to
            // Ams even when BLE never started.
            App::KeyPath::setBleSinks(bleOn && BleHub::amsActive(mode), bleOn && BleHub::hidActive(mode));

            if (bleOn) {
                if (BleHub::amsActive(mode)) {
                    g_ams.begin();                     // AMS callback -> MediaInfo
                    g_ams.setChangeCallback(App::Media::onChange, nullptr);
                    // Pass both: a null here would unregister the head-unit source.

                    App::Media::router().setSources(&g_ams, &App::Media::hu());
                    App::KeyPath::engine().addSink(&App::KeyPath::amsKeys());
                }
                if (BleHub::hidActive(mode)) App::KeyPath::engine().addSink(&App::KeyPath::hidKeys());

#if FEATURE_ANCS
                // ANCS rides the AMS connection.
                if (BleHub::amsActive(mode))
                    AppleNotificationService::RegisterForNotifications(onAncsNotification);
#endif

                // Key classes per destination come from config, editable at runtime
                // (staged in App::KeyPath::routingCfg(), applied by the keys task).
                App::KeyPath::amsKeys().setClasses(s.routeAms);
                App::KeyPath::hidKeys().setClasses(s.routeHid);

                // Begin() builds the GATT tree and needs more stack than setup() has.
                static BleHub::Mode s_mode = mode;
                xTaskCreate([](void*) {
                                // Short name "MEGA" in the advertising packet, full
                                // name in the scan response: "MegaOpen" filled the
                                // 31-byte packet exactly and iPhones never paired
                                // (MeganeCAN's "MCD1" leaves 4 bytes spare).
                                BleHub::Begin(s_mode, "MEGA", "MegaOpen");
                                memMark("ble.begin");   // on this task, so it interleaves
                                // Only after the GATT tree exists. Not critical:
                                // GATT discovery blocks for seconds, and a BLE
                                // fault must never cost the keys an OTA rollback.
                                App::TaskSpec spec;
                                spec.name  = "ble_svc";
                                spec.fn    = bleServiceTask;
                                spec.stack = App::Plan::BLE_STACK;
                                spec.prio  = App::Plan::BLE_PRIO;
                                spec.core  = App::Plan::BLE_CORE;
                                spec.idOut = &g_bleTaskId;
                                g_bleTask = App::spawn(spec);
                                vTaskDelete(nullptr);
                            },
                            "bt_begin", App::Plan::BT_BEGIN_STACK, nullptr, 1, nullptr);
                LOGI("boot", "BLE mode=%s\n", bm.c_str());
            }

            // Warn when the media screen is on but no AMS role will feed it.
#if FEATURE_DISPLAY_CAN
            const bool mediaAuto = App::Ui::mediaAuto();
#else
            const bool mediaAuto = false;
#endif
            if (mediaAuto && !(bleOn && BleHub::amsActive(mode)))
                LOGI("boot", "media screen is ON but bleMode=%s has no AMS "
                              "role — rows will stay empty until it is 'ams' or 'both'\n",
                              bm.c_str());
        }
    }

    TaskHandle_t task() { return g_bleTask; }
    const Sinks::BleKeyWorker& keys() { return g_bleKeys; }
#if FEATURE_ANCS
    const uint32_t* ancsCount() { return &g_ancsCount; }
#endif
}
#endif
