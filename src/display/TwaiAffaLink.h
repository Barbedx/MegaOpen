#pragma once
#include <AffaDisplay.h>
#include "../bus/TwaiBus.h"

// AffaDisplay's CAN seam on this firmware's own TWAI driver (TwaiBus).
//
// Not CanCommonLink: esp32_can hard-codes the driver rx_queue_len = 6, and an
// unanswered panel retransmits at 4 000-10 700 frames/s of one id. Measured on
// the bench: esp32_can 0 frames / busErr 43 806 in 30 s; TwaiBus (queue 64)
// 115 623 frames / busErr 0. CanCommonLink also ignores CAN0.begin()'s return.
//
// Pull port with no buffer of its own: twai_receive() with zero timeout already
// is one. A shallower ring on top would reintroduce the overrun. The one frame
// the panel task's wait woke on (TwaiBus::waitRx) is kept in TwaiBus and comes
// out of recv() first, so sleeping on the bus never reorders or loses a frame.
namespace MegaOpen
{
    class TwaiAffaLink : public affa::ICanLink {
    public:
        // No begin(): main.cpp opens the controller and owns the role decision.
        bool send(const affa::Frame& f) override {
            return trySend(f) == affa::TxDisposition::Accepted;
        }

        // Busy = momentarily full driver queue (a 96-byte screen is 13 frames vs
        // 8 TX slots); the FSM retries it without spending payload or retry budget.
        // Reporting Rejected instead would abort the transfer.
        affa::TxDisposition trySend(const affa::Frame& f) override {
            Frame o;
            o.id       = f.id;
            o.len      = f.len > 8 ? 8 : f.len;
            for (uint8_t i = 0; i < 8; ++i) o.data[i] = i < o.len ? f.data[i] : 0;
            o.extended = f.ext;
            o.source   = Frame::SRC_MM_CAN;

            // This link's own isLive(): a shut TX gate or listen-only is a
            // link that cannot send (Rejected, the library holds it as
            // LinkDown), not a full queue to retry at once.
            if (!isLive()) return affa::TxDisposition::Rejected;
            const TwaiBus::TxResult r = TwaiBus::instance().trySend(o);
            if (r == TwaiBus::TxResult::Ok) { ++_tx; return affa::TxDisposition::Accepted; }
            // Failed with the controller gone (bus-off/recovering/stopped since
            // the check above) is a link that cannot send: Rejected, which the
            // library holds as LinkDown and expires after AFFA_TX_HOLD_MS. Busy
            // there re-armed the head job for ever and wedged the queue.
            if (r == TwaiBus::TxResult::Failed && !isLive())
                return affa::TxDisposition::Rejected;
            // A full driver queue, or a refusal from a controller that still
            // reads RUNNING (a live Rejected would be a terminal SendFailed):
            // Busy, and the next offer sees any state change.
            ++_txBusy;
            return affa::TxDisposition::Busy;
        }

        // AffaDisplayBase::poll() drains `while (_link.recv(f))`; at 100% bus load
        // (1480 frames/s from an unanswered panel) it never empties, so the 1 Hz
        // heartbeat after it never runs and the storm sustains itself. The caller
        // refills this budget once per poll; declined frames stay in the driver queue.
        void newPoll() { _budget = kPerPoll; }

        bool recv(affa::Frame& out) override {
            if (!_budget) return false;
            Frame in;
            if (!TwaiBus::instance().recvOne(in)) return false;
            --_budget;
            out.id  = in.id;
            out.len = in.len > 8 ? 8 : in.len;
            for (uint8_t i = 0; i < 8; ++i) out.data[i] = i < out.len ? in.data[i] : 0;
            out.ext = in.extended;
            ++_rx;
            return true;
        }

        // isLive() gates TX; healthy() means a working controller. Listen-only is
        // healthy (recovery must not tear it down) but not live. So is a TX gate
        // the panel task shut because nobody ACKs (App::Panel, §Step 3): the
        // library then holds renders as LinkDown instead of feeding retries to a
        // bus with no panel on it, and its deaf-link watchdog stands down.
        // running() too, as CanCommonLink does: a BUS_OFF, RECOVERING or STOPPED
        // controller refuses every offer, and the held job must be able to
        // expire.
        bool isLive() const override {
            return TwaiBus::instance().isLive() &&
                   TwaiBus::instance().txEnabled() &&
                   TwaiBus::instance().mode() != TWAI_MODE_LISTEN_ONLY &&
                   TwaiBus::instance().running();
        }
        bool healthy() const override { return TwaiBus::instance().running(); }

        // = the driver's RX queue depth: one poll takes what was waiting, not what
        // arrives during the drain.
        static constexpr uint16_t kPerPoll = 64;

        uint32_t rxFrames() const { return _rx; }
        uint32_t txFrames() const { return _tx; }
        uint32_t txBusy() const   { return _txBusy; }

    private:
        uint32_t _rx = 0, _tx = 0, _txBusy = 0;
        uint16_t _budget = kPerPoll;
    };
}
