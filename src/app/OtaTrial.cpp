#include <Preferences.h>
#include <string.h>
#include "OtaTrial.h"
#include "OtaVerdict.h"
#include "../utils/LogBus.h"
#include <Arduino.h>
#include <esp_ota_ops.h>

// Stops the core marking every image valid at boot (esp32-hal-misc.c); the
// verdict is tick()'s.
extern "C" bool verifyRollbackLater() { return true; }

namespace App
{
    namespace OtaTrial
    {
        namespace
        {
            OtaVerdict s_v;
        }

        // An update that did not stay: the bootloader went back to this image
        // because the new one was never confirmed (it was unhealthy for 180 s,
        // or it was reset in its first 60 s: key off counts). Said once per
        // such image, as a warning, so it reaches the flash log and the push.
        static void saidRollback()
        {
            const esp_partition_t* bad = esp_ota_get_last_invalid_partition();
            esp_app_desc_t d;
            if (!bad || esp_ota_get_partition_description(bad, &d) != ESP_OK) return;
            uint32_t id;
            memcpy(&id, d.app_elf_sha256, sizeof(id));
            Preferences p;
            p.begin("ota", false);
            const bool known = p.getUInt("rbSeen", 0) == id;
            if (!known) p.putUInt("rbSeen", id);
            p.end();
            if (known) return;
            LOGW("ota", "the last update did not stay: the image in %s (built %s %s) was not confirmed in its "
                        "trial (unhealthy, or reset in its first %us: key off counts). Running the previous one",
                 bad->label, d.date, d.time, (unsigned)(OtaVerdict::CONFIRM_MS / 1000));
        }

        void begin()
        {
            const esp_partition_t* run = esp_ota_get_running_partition();
            esp_ota_img_states_t st;
            const bool pending = run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
                                 st == ESP_OTA_IMG_PENDING_VERIFY;
            s_v.begin(pending);
            if (pending)
                LOGI("ota", "on trial: confirmed after %us healthy, rolled back after %us",
                               (unsigned)(OtaVerdict::CONFIRM_MS / 1000),
                               (unsigned)(OtaVerdict::TRIAL_MS / 1000));
            saidRollback();
        }

        bool onTrial() { return s_v.onTrial(); }

        void tick(uint32_t nowMs, bool healthy)
        {
            switch (s_v.tick(nowMs, healthy)) {
                case OtaVerdict::Act::Confirm:
                    esp_ota_mark_app_valid_cancel_rollback();
                    LOGI("ota", "CONFIRMED (healthy %us)",
                                   (unsigned)(OtaVerdict::CONFIRM_MS / 1000));
                    break;
                case OtaVerdict::Act::Revert:
                    LOGI("ota", "never became healthy - rolling back");
                    // The line has to reach the flash log before the reboot.
                    LogBus::flushAndWait(300);
                    esp_ota_mark_app_invalid_rollback_and_reboot();
                    break;
                case OtaVerdict::Act::None:
                    break;
            }
        }
    }
}
