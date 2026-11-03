#include "StockRadio.h"
#if FEATURE_DISPLAY_CAN
#include <AffaDisplay.h>
#include "PanelOwner.h"
#include "../display/RadioCode.h"
#include "../utils/LogBus.h"

namespace App
{
    namespace StockRadio
    {
        namespace
        {
            Cfg::Config*        s_cfg = nullptr;
            MegaOpen::RadioCode s_code;

            // The panel's own key frame onto the bus, from the panel task:
            // no ticket, no PanelOps.
            bool send(uint16_t wire, bool hold, void*)
            {
                const affa::Submitted s = App::Panel::lib().pressKey(
                    (affa::Key)wire, hold ? affa::KeyEdge::Hold : affa::KeyEdge::Click,
                    affa::KeySource::Wire);
                return s.result == affa::Result::Ok;
            }

            // The setting as the page last saved it. A torn read of five
            // bytes is the worst case (Cfg::Text): the next pass reads it
            // whole; never the blocking lock on this task.
            void readSetting(uint32_t now)
            {
                static uint32_t s_gen = 0xFFFFFFFFu, s_genAt = 0;
                if (s_gen == s_cfg->generation() || (uint32_t)(now - s_genAt) < 1000) return;
                s_genAt = now;
                if (!s_cfg->tryLock()) return;
                s_gen = s_cfg->generation();
                const bool was = s_code.enabled();
                s_code.setCode(s_cfg->s().radioCode.c_str());
                s_cfg->unlock();
                if (was != s_code.enabled())
                    LOGI("code", "radio code: %s", s_code.enabled() ? "set, typed when the radio asks" : "off");
            }

            void report()
            {
                using RC = MegaOpen::RadioCode;
                const RC::Event e = s_code.takeEvent();
                if (e == RC::Event::None) return;
                const RC::View v = s_code.view();
                switch (e) {
                    case RC::Event::Prompt:
                        LOGI("code", "the radio asks for its code: typing it (attempt %u)", (unsigned)(v.attempts + 1));
                        break;
                    case RC::Event::Typed:
                        LOGI("code", "%u keys sent; the radio's answer in %u s", (unsigned)v.sent,
                             (unsigned)(RC::RESULT_MS / 1000));
                        break;
                    case RC::Event::Accepted:
                        LOGI("code", "accepted: the prompt is gone");
                        break;
                    case RC::Event::Rejected:
                        LOGW("code", "REJECTED: the radio asks again; check the code on the Плата page (not typed again for %u s)",
                             (unsigned)(RC::COOLDOWN_MS / 1000));
                        break;
                    case RC::Event::Skipped:
                        LOGW("code", "the radio asks again within %u s of the last attempt: not typed (it waits longer after each wrong code)",
                             (unsigned)(RC::COOLDOWN_MS / 1000));
                        break;
                    case RC::Event::NoBus:
                        LOGW("code", "no key reached the bus after %u tries: given up for this prompt",
                             (unsigned)RC::SEND_TRIES);
                        break;
                    default:
                        break;
                }
            }
        }

        void begin(Cfg::Config& cfg)
        {
            s_cfg = &cfg;
            s_code.begin(send, nullptr);
            s_code.setCode(cfg.s().radioCode.c_str());
            if (s_code.enabled()) LOGI("code", "radio code set: typed when the radio asks for it");
        }

        bool active() { return s_cfg != nullptr; }

        void onRx(uint32_t id, const uint8_t* d, uint8_t len, uint32_t nowMs)
        {
            if (!s_cfg) return;
            s_code.onRx(id, d, len, nowMs);
        }

        void step(uint32_t nowMs)
        {
            if (!s_cfg) return;
            readSetting(nowMs);
            s_code.tick(nowMs);
            report();
        }
    }
}
#endif
