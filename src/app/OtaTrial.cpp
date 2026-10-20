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

        void begin()
        {
            const esp_partition_t* run = esp_ota_get_running_partition();
            esp_ota_img_states_t st;
            const bool pending = run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
                                 st == ESP_OTA_IMG_PENDING_VERIFY;
            s_v.begin(pending);
            if (pending)
                LogBus::printf("[ota] on trial: confirmed after %us healthy, rolled back after %us",
                               (unsigned)(OtaVerdict::CONFIRM_MS / 1000),
                               (unsigned)(OtaVerdict::TRIAL_MS / 1000));
        }

        bool onTrial() { return s_v.onTrial(); }

        void tick(uint32_t nowMs, bool healthy)
        {
            switch (s_v.tick(nowMs, healthy)) {
                case OtaVerdict::Act::Confirm:
                    esp_ota_mark_app_valid_cancel_rollback();
                    LogBus::printf("[ota] CONFIRMED (healthy %us)",
                                   (unsigned)(OtaVerdict::CONFIRM_MS / 1000));
                    break;
                case OtaVerdict::Act::Revert:
                    LogBus::line("[ota] never became healthy - rolling back");
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
