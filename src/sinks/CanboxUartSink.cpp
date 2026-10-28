#include "CanboxUartSink.h"

namespace Sinks
{
    using namespace Keys;

    bool CanboxUartSink::sendKey(uint8_t code, uint8_t state)
    {
        if (!_enabled || (!_write && !_writeOk) || code == 0) return false;
        uint8_t frame[Canbox::HU_FRAME_MAX];
        const uint8_t n = proto().keyFrame(code, state, frame);
        if (!n) return false;
        bool ok = true;
        if (_writeOk) ok = _writeOk(frame, n, _ctx);
        else          _write(frame, n, _ctx);
        // Counted only when taken: a refused frame did not reach the unit.
        if (ok) ++_framesSent;
        return ok;
    }

    void CanboxUartSink::setEnabled(bool on)
    {
        if (_enabled && !on) {
            // While the link can still write: a release scheduled for later
            // would otherwise be dropped by tick() and the key stay down.
            for (auto& p : _pending) {
                if (!p.used) continue;
                sendKey(p.code, Canbox::KEY_RELEASE);
                p.used = false;
            }
            if (_muteDown) sendKey(proto().keyCodeFor(Keys::Action::Mute), Canbox::KEY_RELEASE);
            _muteDown = false;
        }
        _enabled = on;
    }

    void CanboxUartSink::schedule(uint8_t code, uint32_t dueMs)
    {
        for (auto& p : _pending) {
            if (!p.used) { p = Pending{code, dueMs, true}; return; }
        }
        // All slots busy: release the oldest early rather than risk a stuck key.
        uint8_t oldest = 0;
        for (uint8_t i = 1; i < MAX_PENDING; ++i)
            if ((int32_t)(_pending[i].dueMs - _pending[oldest].dueMs) < 0) oldest = i;
        sendKey(_pending[oldest].code, Canbox::KEY_RELEASE);
        _pending[oldest] = Pending{code, dueMs, true};
    }

    void CanboxUartSink::releasePendingOf(uint8_t code)
    {
        for (auto& p : _pending) {
            if (!p.used || p.code != code) continue;
            sendKey(code, Canbox::KEY_RELEASE);
            p.used = false;
        }
    }

    bool CanboxUartSink::sendPress(uint8_t code, uint8_t state, uint32_t nowMs)
    {
        // Two presses of one code with no release between them read as one
        // long hold (a fast spin of a detent on a plain code).
        releasePendingOf(code);
        if (!sendKey(code, state)) return false;
        _lastPressCode = code;
        _lastPressMs   = nowMs;
        return true;
    }

    bool CanboxUartSink::longMute(const Event& e) const
    {
        return e.action == Keys::Action::Mute && !e.codeHint && proto().muteIsLong();
    }

    // The only place the translation lives; onKey() and the popup both use it.
    CanboxUartSink::Explain CanboxUartSink::explain(const Event& e) const
    {
        Explain x;

        if (!_enabled) { x.why = "uart off"; return x; }

        const uint8_t cls = Keys::classOf(e.action);
        if (cls == Keys::CLASS_NONE) { x.why = "no class";   return x; }
        if (!(_mask & cls))          { x.why = "not routed"; return x; }

        const uint8_t code = e.codeHint ? e.codeHint : proto().keyCodeFor(e.action);
        if (!code) { x.why = "no code"; return x; }
        x.code = code;

        if (isEncoder(e.action)) {
            if (e.state != State::Press) { x.why = "wheel: press only"; return x; }

            // Rotary codes (0x81-0x86): second byte is a detent count, no release.
            // Other codes need press + scheduled release or they stick down.
            if (proto().isKnobCode(code)) {
                x.sends = true; x.knob = true; x.state = 1;   // one detent
                return x;
            }
            x.sends = true; x.state = Canbox::KEY_PRESS; x.release = true;
            return x;
        }

        // Mute held: a long press of its code (RAV4: SOURCE), not a code of
        // its own. A Long (an asLong map row) is the same press; a Long after
        // a Press that already holds it is not sent twice.
        if (longMute(e)) {
            if (e.state == State::Press || (e.state == State::Long && !_muteDown)) {
                x.sends = true; x.state = Canbox::KEY_LONG; return x;
            }
            if (e.state == State::Release) { x.sends = true; x.state = Canbox::KEY_RELEASE; return x; }
            x.why = e.state == State::Long ? "mute: held" : "mute: press/release only";
            return x;
        }

        x.sends = true;
        switch (e.state) {
            case State::Press:   x.state = Canbox::KEY_PRESS;   break;
            case State::Long:    x.state = Canbox::KEY_LONG;    break;
            case State::Repeat:  x.state = Canbox::KEY_PRESS;   break;
            case State::Release: x.state = Canbox::KEY_RELEASE; break;
        }
        return x;
    }

    void CanboxUartSink::onKey(const Event& e)
    {
        const Explain x = explain(e);
        if (!x.sends) return;

        const bool mute = longMute(e);
        if (mute) _muteDown = x.state != Canbox::KEY_RELEASE;

        if (x.knob) { sendKey(x.code, x.state); return; }

        const uint32_t now = nowOr(e.tMs);
        if (x.state == Canbox::KEY_RELEASE) {
            // On the heels of its own press (a Tap, a panel key, a macro's
            // Action): scheduled MIN_GAP_MS after the press instead, which some
            // head units need to see it at all (IKeySink.h). One already
            // scheduled for this code covers it.
            if (x.code == _lastPressCode &&
                (int32_t)(now - (_lastPressMs + Keys::MIN_GAP_MS)) < 0) {
                for (const auto& p : _pending)
                    if (p.used && p.code == x.code) return;
                schedule(x.code, _lastPressMs + Keys::MIN_GAP_MS);
                return;
            }
            // A release scheduled for this code is this one, sent now.
            for (auto& p : _pending)
                if (p.used && p.code == x.code) p.used = false;
            sendKey(x.code, x.state);
            return;
        }

        if (!sendPress(x.code, x.state, now)) return;
        // Scheduled here: explain() must stay side-effect free. From the
        // source time, but never sooner than MIN_GAP_MS from now (IKeySink.h).
        if (x.release) schedule(x.code, Keys::releaseDue(e.tMs, _pulseMs, now));
    }

    void CanboxUartSink::sendTap(uint8_t code, uint8_t state)
    {
        // Knob codes carry a detent count: no release. A release is just sent.
        if (proto().isKnobCode(code) || state == Canbox::KEY_RELEASE) { sendKey(code, state); return; }
        const uint32_t now = nowOr(0);
        if (sendPress(code, state, now)) schedule(code, now + Keys::MIN_GAP_MS);
    }

    bool CanboxUartSink::nextDue(uint32_t& dueMs) const
    {
        bool any = false;
        for (const auto& p : _pending) {
            if (!p.used) continue;
            if (!any || (int32_t)(p.dueMs - dueMs) < 0) dueMs = p.dueMs;
            any = true;
        }
        return any;
    }

    void CanboxUartSink::tick(uint32_t nowMs)
    {
        for (auto& p : _pending) {
            if (!p.used || (int32_t)(nowMs - p.dueMs) < 0) continue;
            // A release the writer refused stays scheduled and is tried at
            // the next tick; only a link switched off or a writer refusing
            // for RETRY_MAX_MS gives it up.
            if (sendKey(p.code, Canbox::KEY_RELEASE) || !_enabled ||
                (uint32_t)(nowMs - p.dueMs) >= RETRY_MAX_MS)
                p.used = false;
        }
    }
}
