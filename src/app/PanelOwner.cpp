#include "PanelOwner.h"
#if FEATURE_DISPLAY_CAN
#include "../diag/CanBench.h"
#include "../display/AffaPort.h"
#include "../utils/LogBus.h"

namespace App
{
    namespace Panel
    {
        namespace
        {
            Setup        s_s;
            TaskHandle_t s_task = nullptr;
            uint8_t      s_features = 0;

            Snapshot<MegaOpen::PanelStatus> s_status;

            // The pump's timing. s_pollGapUs is read by the key callback inside
            // poll(), on this task; the window max by the [panel] line.
            uint32_t s_pollAtUs = 0, s_pollGapUs = 0, s_pollLateWinUs = 0;
            uint32_t s_polls = 0;

            // Delivery verdicts, counted on this task (onComplete).
            uint32_t     s_delivered = 0, s_superseded = 0, s_failed = 0;
            affa::Result s_lastDelivery = affa::Result::Ok;
            uint16_t     s_failStreak = 0;
            // PanelOps::resyncs() as last seen: a resync clears the streak.
            uint32_t     s_resyncsSeen = 0;

            // Both edges of every sync change, for the ui task (PanelStatus).
            uint32_t s_syncChanges = 0;

            void* currentTask() { return (void*)xTaskGetCurrentTaskHandle(); }

            // Inside poll(). The ui re-owes the clock and resends the pane on
            // every change; it sees the count, not the callback.
            void onSync(affa::SyncState, void*)
            {
                ++s_syncChanges;
            }

            void onComplete(affa::TxTicket t, affa::Result r, void*)
            {
                if (r == affa::Result::Ok)           { ++s_delivered; s_failStreak = 0; }
                // Aborted (superseded) and Cancelled (resync) are not transport
                // faults and must not trip the breaker.
                else if (r == affa::Result::Aborted ||
                         r == affa::Result::Cancelled) ++s_superseded;
                else                                 { ++s_failed; ++s_failStreak; }
                s_lastDelivery = r;
                // Every verdict goes on; the ui task keeps the ones it waits on.
                if (s_s.hooks.onCompletion) {
                    UiWaits::Completion c;
                    c.ticket = t;
                    c.result = (uint8_t)r;
                    s_s.hooks.onCompletion(c);
                }
            }

            void publish()
            {
                affa::AffaDisplayBase& d = *s_s.display;
                MegaOpen::PanelStatus p;
                p.attached     = true;
                p.synced       = d.synced();
                p.registered   = d.registered();
                p.busy         = d.busy();
                p.passive      = d.passive();
                p.syncState    = (uint8_t)d.syncState();
                p.phase        = (uint8_t)d.phase();
                p.queued       = d.queued();
                p.lastResult   = (uint8_t)d.lastResult();
                p.lastRendered = (uint8_t)d.lastRendered();
                p.lastLoss     = (uint8_t)d.lastLossReason();
                p.features     = s_features;
                p.lastRenderedMs = d.lastRenderedMs();
                p.sessionsLost   = d.sessionsLost();
                const affa::AffaDisplayBase::LinkHealth h = d.linkHealth();
                p.linkFlaps      = h.flaps;
                p.linkRecoveries = h.recoveries;
                p.linkFailures   = h.failures;
                p.linkDownMs     = h.downMs;
                p.delivered    = s_delivered;
                p.superseded   = s_superseded;
                p.failed       = s_failed;
                p.lastDelivery = (uint8_t)s_lastDelivery;
                p.failStreak   = s_failStreak;
                p.polls         = s_polls;
                p.foreignPolls  = d.foreignPolls();
                p.pollLateMaxUs = s_pollLateWinUs;
                p.queuePosted   = s_s.ops ? s_s.ops->posted() : 0;
                p.queueDropped  = d.dispatchDropped();
                // The link's own TX gate (affa::TxGate), sampled inside poll().
                if (s_s.link) {
                    p.txGated = s_s.link->gate().shut();
                    p.txGates = s_s.link->gate().shuts();
                }
                p.carminat      = s_s.carminat != nullptr;
                p.syncChanges   = s_syncChanges;
                p.yielded       = s_s.ops ? s_s.ops->yielded() : 0;
                s_status.publish(p);
            }
        }

        void begin(const Setup& s)
        {
            s_s = s;
            s_task = xTaskGetCurrentTaskHandle();
            if (!s_s.display) return;
            affa::AffaDisplayBase& d = *s_s.display;
            // poll() from any other task is now a counted no-op (foreignPolls).
            d.setPollOwner((void*)s_task, currentTask);
            d.onSync(&onSync, nullptr);
            d.onComplete(&onComplete, nullptr);

            using F = affa::Feature;
            using namespace MegaOpen;
            s_features = (d.supports(F::Text)       ? PF_TEXT : 0)
                       | (d.supports(F::Time)       ? PF_TIME : 0)
                       | (d.supports(F::Power)      ? PF_POWER : 0)
                       | (d.supports(F::Menu)       ? PF_MENU : 0)
                       | (d.supports(F::Popup)      ? PF_POPUP : 0)
                       | (d.supports(F::Fullscreen) ? PF_FULLSCREEN : 0)
                       | (d.supports(F::ConfirmBox) ? PF_CONFIRM : 0)
                       | (d.supports(F::InfoPopup)  ? PF_INFO : 0);
            publish();
        }

        affa::AffaDisplayBase& lib() { return *s_s.display; }

        bool present() { return s_s.display != nullptr; }

        void pump(uint32_t)
        {
            if (!s_s.display) return;
            affa::AffaDisplayBase& d = lib();
            // A resync posted since the last pass gives the fresh link a clean
            // run at the breaker.
            if (s_s.ops && s_s.ops->resyncs() != s_resyncsSeen) {
                s_resyncsSeen = s_s.ops->resyncs();
                s_failStreak = 0;
            }
            // The gap since the last poll() began: a key decoded in this poll
            // could have waited that long in the TWAI queue. The key callback
            // posts it with the key, and the keys task reports the worst.
            const uint32_t t = micros();
            s_pollGapUs = s_pollAtUs ? t - s_pollAtUs : 0;
            s_pollAtUs = t;
            if (s_pollGapUs > s_pollLateWinUs) s_pollLateWinUs = s_pollGapUs;
            // Inside, in the library's order: the link's housekeeping (the TX
            // gate, bus-off recovery), the keys (at most AFFA_RX_BUDGET frames,
            // so a storm cannot starve the rest), what other tasks posted, then
            // the transmit pump.
            d.poll();
            ++s_polls;
            publish();
        }

        // library gap: AffaTask sleeps a fixed period, not in TwaiLink::waitRx(), so this task polls.
        void wait()
        {
            uint32_t ms = 2;
            // Gated, the library holds its jobs as LinkDown and stays busy()
            // until their hold runs out: a pass every millisecond would do
            // nothing for that whole window. A frame from the panel still
            // wakes us. busy() counts the renders posted and not yet admitted.
            if (s_s.display && s_s.link && !s_s.link->gate().shut() && lib().busy()) ms = 1;
            if (s_s.link) s_s.link->waitRx(ms);
            else          vTaskDelay(pdMS_TO_TICKS(ms));
        }

        void logWindow()
        {
            if (!s_s.display) return;
            const MegaOpen::PanelStatus p = s_status.read();
            const uint32_t inst = Diag::CanBench::status().installs;
            const uint32_t reinstalls = inst ? inst - 1 : 0;
            // foreignPolls should be 0, and pollLateMax a few ms: the gap
            // between two poll()s, which is how long a key frame can wait.
            // dropped = renders the library's ring refused; yielded = repaints
            // a person's screen overtook; reinstalls = TWAI driver installs
            // after the first (bus-off or a deaf controller, the library's).
            LogBus::printf("[panel] foreignPolls=%u pollLateMax=%uus polls=%u queued=%u "
                           "posted=%u dropped=%u delivered=%u superseded=%u failed=%u "
                           "failStreak=%u txGated=%u gates=%u reinstalls=%u yielded=%u",
                           (unsigned)p.foreignPolls, (unsigned)p.pollLateMaxUs,
                           (unsigned)p.polls, (unsigned)p.queued,
                           (unsigned)p.queuePosted, (unsigned)p.queueDropped,
                           (unsigned)p.delivered, (unsigned)p.superseded, (unsigned)p.failed,
                           (unsigned)p.failStreak, p.txGated ? 1u : 0u,
                           (unsigned)p.txGates, (unsigned)reinstalls,
                           (unsigned)p.yielded);
            s_pollLateWinUs = 0;
        }

        MegaOpen::PanelStatus status() { return s_status.read(); }
        const Snapshot<MegaOpen::PanelStatus>& statusSnapshot() { return s_status; }
        uint32_t pollGapUs() { return s_pollGapUs; }
    }
}
#endif
