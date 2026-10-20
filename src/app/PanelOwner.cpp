#include "PanelOwner.h"
#if FEATURE_DISPLAY_CAN
#include "TxGate.h"
#include "../bus/TwaiBus.h"
#include "../diag/CanBench.h"
#include "../display/AffaPort.h"
#include "../utils/LogBus.h"
#include <esp_system.h>

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

            TxGate   s_gate;
            uint32_t s_gateSeq = 0;

            // Both edges of every sync change, for the ui task (PanelStatus).
            uint32_t s_syncChanges = 0;

            void* currentTask() { return (void*)xTaskGetCurrentTaskHandle(); }

            // The owner rule, checked where it can be broken: every route to
            // the library goes through lib() or carminat().
            inline void ownerCheck()
            {
#if AFFA_OWNER_ASSERT
                if (s_task && xTaskGetCurrentTaskHandle() != s_task)
                    esp_system_abort("App::Panel: AffaDisplay called off the panel task");
#endif
            }

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
                // A tracked request's ticket becomes its Completion, and the
                // pane slot it borrowed is free again.
                if (s_s.box) s_s.box->libComplete(t, r);
            }

            void onReport(const MegaOpen::AffaMailbox::Req& r, affa::Result res, void*)
            {
                // drain() already ran the library's begin(): give the fresh
                // link a clean run at the breaker too.
                if (r.op == MegaOpen::AffaMailbox::Op::Resync) s_failStreak = 0;
                // Acceptance verdict of a posted render (delivery is reported
                // separately). The ui task's own requests come back to it as
                // completions and it says what matters about them, rate-limited:
                // logging each refused repaint here would flood the log.
                if (res != affa::Result::Ok && !r.track)
                    LogBus::printf("[disp] %s -> %s", MegaOpen::AffaMailbox::opName(r.op),
                                   MegaOpen::resultName(res));
            }

            void onCompletion(const MailboxIds::Completion& c, void*)
            {
                if (s_s.hooks.onCompletion) s_s.hooks.onCompletion(c);
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
                p.queuePosted   = s_s.box ? s_s.box->posted() : 0;
                p.queueDropped  = s_s.box ? s_s.box->dropped() : 0;
                p.txGated    = s_gate.shut();
                p.txGates    = s_gate.shuts();
                const uint32_t inst = TwaiBus::instance().installs();
                p.reinstalls = inst ? inst - 1 : 0;
                p.carminat      = s_s.carminat != nullptr;
                p.syncChanges   = s_syncChanges;
                if (s_s.box) {
                    p.clears        = s_s.box->clears();
                    p.resyncsAsked  = s_s.box->resyncs();
                    p.deliberateSeq = s_s.box->deliberateSeq();
                    p.ticketsMapped = s_s.box->tickets().used();
                    p.synthesised   = s_s.box->tickets().synthesised();
                    p.yielded       = s_s.box->yielded();
                }
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
            if (s_s.box) {
                s_s.box->onReport(&onReport, nullptr);
                s_s.box->onCompletion(&onCompletion, nullptr);
            }

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

        affa::AffaDisplayBase& lib()
        {
            ownerCheck();
            return *s_s.display;
        }

        affa::CarminatDisplay* carminat()
        {
            ownerCheck();
            return s_s.carminat;
        }

        bool present() { return s_s.display != nullptr; }

        void pump(uint32_t)
        {
            if (!s_s.display) return;
            affa::AffaDisplayBase& d = lib();
            // Bus-off recovery in the display role (TwaiBus::poll() is not
            // called here: it would steal the library's frames).
            TwaiBus::instance().maintain();
            // Other tasks' requests first, then the pump: same-pass delivery.
            // Except when a frame woke this pass (it may be a panel key): then
            // poll() decodes it first, and the drain (up to 16 renders plus
            // the rows, ~1 ms on a burst) goes after it, with a second poll()
            // so the renders still leave this pass.
            const bool rxFirst = TwaiBus::instance().stashed();
            if (s_s.box && !rxFirst) s_s.box->drain(d);
            // Refill the RX budget, or poll() never reaches TX on a saturated
            // bus (TwaiAffaLink.h).
            if (s_s.link) s_s.link->newPoll();
            // The gap since the last poll() began: a key decoded in this poll
            // could have waited that long in the TWAI queue. The key callback
            // posts it with the key, and the keys task reports the worst.
            const uint32_t t = micros();
            s_pollGapUs = s_pollAtUs ? t - s_pollAtUs : 0;
            s_pollAtUs = t;
            if (s_pollGapUs > s_pollLateWinUs) s_pollLateWinUs = s_pollGapUs;
            d.poll();
            ++s_polls;
            // The RX budget is not refilled: this one is for the TX pump.
            if (s_s.box && rxFirst && s_s.box->drain(d)) d.poll();
            publish();
        }

        void serviceTx(uint32_t nowMs)
        {
            if (!s_s.display) return;
            const uint32_t seq = Diag::CanBench::seq();
            if (seq == s_gateSeq) return;
            s_gateSeq = seq;

            TwaiBus& bus = TwaiBus::instance();
            const twai_status_info_t& st = Diag::CanBench::info();
            const bool valid = Diag::CanBench::infoValid();
            TxGate::Sample smp;
            smp.nowMs      = nowMs;
            smp.tec        = valid ? st.tx_error_counter : 0;
            smp.txFailed   = valid ? st.tx_failed_count : 0;
            smp.rxFrames   = bus.rxCount();
            // Listen-only transmits nothing, and a missing driver has nothing
            // to gate: the gate opens and forgets.
            smp.mayTx      = valid && bus.isLive() && bus.mode() != TWAI_MODE_LISTEN_ONLY;

            switch (s_gate.tick(smp)) {
                case TxGate::Act::Shut:
                    // Off first, so nothing new is offered; then the frame the
                    // controller keeps retrying goes too (TwaiBus::abortTx).
                    bus.setTxEnabled(false);
                    {
                        const bool aborted = bus.abortTx();
                        LogBus::printf("[panel] TX gated: nothing heard for %u ms and nobody ACKs "
                                       "(tec=%u rec=%u txfail=%u aborted=%u) - retry in %u ms",
                                       (unsigned)TxGate::NO_RX_MS, (unsigned)st.tx_error_counter,
                                       (unsigned)st.rx_error_counter, (unsigned)st.tx_failed_count,
                                       aborted ? 1u : 0u, (unsigned)s_gate.backoffMs());
                    }
                    break;
                case TxGate::Act::Open:
                    bus.setTxEnabled(true);
                    LogBus::printf("[panel] TX open again (%s)",
                                   s_gate.probing() ? "probing" : "the bus is talking");
                    break;
                default:
                    break;
            }
        }

        TickType_t waitTicks()
        {
            if (!s_s.display) return pdMS_TO_TICKS(2);
            // Gated, the library holds its jobs as LinkDown and stays busy()
            // until their hold runs out: a pass every tick would do nothing
            // for that whole window. A frame from the panel still wakes us.
            if (!TwaiBus::instance().txEnabled()) return pdMS_TO_TICKS(2);
            const bool work = lib().busy() || (s_s.box && s_s.box->pending());
            return work ? (TickType_t)1 : pdMS_TO_TICKS(2);
        }

        void logWindow()
        {
            if (!s_s.display) return;
            const MegaOpen::PanelStatus p = s_status.read();
            // foreignPolls should be 0, and pollLateMax a few ms: the gap
            // between two poll()s, which is how long a key frame can wait.
            // mapped = tickets the ui still waits on (a handful at most; one
            // that only climbs is a verdict the library never gave); synth and
            // yielded = requests answered without the wire.
            LogBus::printf("[panel] foreignPolls=%u pollLateMax=%uus polls=%u queued=%u "
                           "posted=%u dropped=%u delivered=%u superseded=%u failed=%u "
                           "failStreak=%u txGated=%u gates=%u reinstalls=%u "
                           "mapped=%u synth=%u yielded=%u",
                           (unsigned)p.foreignPolls, (unsigned)p.pollLateMaxUs,
                           (unsigned)p.polls, (unsigned)p.queued,
                           (unsigned)p.queuePosted, (unsigned)p.queueDropped,
                           (unsigned)p.delivered, (unsigned)p.superseded, (unsigned)p.failed,
                           (unsigned)p.failStreak, p.txGated ? 1u : 0u,
                           (unsigned)p.txGates, (unsigned)p.reinstalls,
                           (unsigned)p.ticketsMapped, (unsigned)p.synthesised,
                           (unsigned)p.yielded);
            s_pollLateWinUs = 0;
        }

        MegaOpen::PanelStatus status() { return s_status.read(); }
        const Snapshot<MegaOpen::PanelStatus>& statusSnapshot() { return s_status; }
        uint32_t pollGapUs() { return s_pollGapUs; }
    }
}
#endif
