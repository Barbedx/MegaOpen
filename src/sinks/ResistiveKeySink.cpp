#include "ResistiveKeySink.h"

namespace Sinks
{
    using namespace Keys;

    void ResistiveKeySink::begin(ApplyFn apply, void* ctx, ResistiveMap* map)
    {
        _apply = apply;
        _ctx   = ctx;
        _map   = map;
        // Open explicitly: after a reset the GPIO state is whatever the bootloader left.
        for (uint8_t c = 0; c < ResistiveMap::CHANNELS; ++c) {
            _ch[c] = Channel{};
            if (_apply) _apply(c, 0, _ctx);
        }
    }

    void ResistiveKeySink::setEnabled(bool on)
    {
        if (_enabled == on) return;
        _enabled = on;
        // Off must release, or the resistance stays on the line.
        if (!on) allOff();
    }

    void ResistiveKeySink::allOff()
    {
        for (uint8_t c = 0; c < ResistiveMap::CHANNELS; ++c) {
            Channel& ch = _ch[c];
            ch.queue = 0;
            ch.queueAction = Action::None;
            ch.owner = Action::None;
            ch.held = false;
            ch.learn = false;
            ch.pending = Action::None;
            ch.holdEndMs = 0;
            if (ch.mask && _apply) _apply(c, 0, _ctx);
            ch.mask = 0;
        }
    }

    void ResistiveKeySink::learnAssert(uint8_t ch, uint8_t mask, uint32_t ms, uint32_t nowMs)
    {
        if (ch >= ResistiveMap::CHANNELS || !mask || !_map) return;

        Channel& c = _ch[ch];
        // Stop everything: a detent draining mid-hold would put a second voltage
        // on the line while the head unit measures.
        c.queue = 0;
        c.queueAction = Action::None;
        c.pending = Action::None;
        if (c.mask && _apply) _apply(ch, 0, _ctx);

        c.mask    = mask;
        c.owner   = Action::None;
        c.held    = false;
        c.learn   = true;
        c.untilMs = nowMs + (ms ? ms : LEARN_MS);
        // Not engage(): ignores routing and enable, and isn't counted as a press.
        if (_apply) _apply(ch, mask, _ctx);
    }

    bool ResistiveKeySink::learnAction(Action a, uint32_t ms, uint32_t nowMs)
    {
        if (!_map) return false;
        const ResistiveMap::Slot s = _map->slot(a);
        if (!s.mask) return false;
        if (_map->ladder(s.ch).ohmsFor(s.mask) == ResistiveLadder::OPEN) return false;
        learnAssert(s.ch, s.mask, ms, nowMs);
        return true;
    }

    void ResistiveKeySink::learnCancel(uint32_t nowMs)
    {
        for (uint8_t c = 0; c < ResistiveMap::CHANNELS; ++c) {
            if (!_ch[c].learn) continue;
            _ch[c].learn = false;
            open(c, nowMs);
        }
    }

    uint16_t ResistiveKeySink::liveOhms(uint8_t ch) const
    {
        if (!_map || ch >= ResistiveMap::CHANNELS) return ResistiveLadder::OPEN;
        return _map->ladder(ch).ohmsFor(_ch[ch].mask);
    }

    // ---- the one resolver ----------------------------------------------------
    ResistiveKeySink::Explain ResistiveKeySink::explain(const Event& e) const
    {
        Explain x;
        if (!_enabled)          { x.why = "res off";   return x; }
        if (!_map)              { x.why = "no map";    return x; }

        const uint8_t cls = classOf(e.action);
        if (cls == CLASS_NONE)  { x.why = "no class";  return x; }
        if (!(_mask & cls))     { x.why = "not routed"; return x; }

        const ResistiveMap::Slot s = _map->slot(e.action);
        if (!s.mask)            { x.why = "no rung";   return x; }

        x.ch   = s.ch;
        x.mask = s.mask;
        x.ohms = _map->ladder(s.ch).ohmsFor(s.mask);
        // Rung on unpopulated legs: report it, don't look like a routing problem.
        if (x.ohms == ResistiveLadder::OPEN) { x.mask = 0; x.why = "legs unfitted"; return x; }

        if (isEncoder(e.action)) {
            if (e.state != State::Press) { x.why = "wheel: press only"; return x; }
            x.acts = true; x.queued = true;
            return x;
        }

        // Long only matters when a long rung is configured.
        if (e.state == State::Long && s.longMask) {
            x.mask = s.longMask;
            x.ohms = _map->ladder(s.ch).ohmsFor(s.longMask);
            if (x.ohms == ResistiveLadder::OPEN) { x.mask = 0; x.why = "long rung unfitted"; return x; }
            x.acts = true;
            return x;
        }

        // Levels, not events: the head unit does its own long/repeat on the held rung.
        if (e.state == State::Long || e.state == State::Repeat) {
            x.why = "held already"; return x;
        }
        x.acts = true;
        return x;
    }

    // ---- timing --------------------------------------------------------------
    // Compress shortens pulse and gap linearly with queue depth, down to the
    // floors (a divisor would hit the floor after two detents).
    uint16_t ResistiveKeySink::pulseFor(uint8_t depth) const
    {
        const ResistiveMap::Timing& t = _map->timing();
        if (_map->spin() != ResistiveMap::Spin::Compress || depth < 2) return t.pulseMs;
        const uint16_t step = (uint16_t)((t.pulseMs > t.minPulseMs ? t.pulseMs - t.minPulseMs : 0) / 4);
        const uint32_t cut  = (uint32_t)step * (depth - 1);
        return (cut >= (uint32_t)(t.pulseMs - t.minPulseMs)) ? t.minPulseMs
                                                             : (uint16_t)(t.pulseMs - cut);
    }

    uint16_t ResistiveKeySink::gapFor(uint8_t depth) const
    {
        const ResistiveMap::Timing& t = _map->timing();
        if (_map->spin() != ResistiveMap::Spin::Compress || depth < 2) return t.gapMs;
        const uint16_t step = (uint16_t)((t.gapMs > t.minGapMs ? t.gapMs - t.minGapMs : 0) / 4);
        const uint32_t cut  = (uint32_t)step * (depth - 1);
        return (cut >= (uint32_t)(t.gapMs - t.minGapMs)) ? t.minGapMs
                                                         : (uint16_t)(t.gapMs - cut);
    }

    void ResistiveKeySink::engage(uint8_t ch, uint8_t mask, Action a, bool held, uint32_t nowMs)
    {
        Channel& c = _ch[ch];
        c.mask  = mask;
        c.owner = a;
        c.held  = held;
        // For a held button the deadline is the stuck-key guard against a lost Release.
        c.untilMs = nowMs + (held ? _map->timing().maxHoldMs : pulseFor(c.queue));
        if (_apply) _apply(ch, mask, _ctx);
        ++_presses;
    }

    void ResistiveKeySink::open(uint8_t ch, uint32_t nowMs)
    {
        Channel& c = _ch[ch];
        if (c.mask && _apply) _apply(ch, 0, _ctx);
        c.mask  = 0;
        c.owner = Action::None;
        c.held  = false;
        c.freeAtMs = nowMs + gapFor(c.queue);
    }

    void ResistiveKeySink::startNextDetent(uint8_t ch, uint32_t nowMs)
    {
        Channel& c = _ch[ch];
        if (!c.queue || !_map) return;
        const ResistiveMap::Slot s = _map->slot(c.queueAction);
        if (!s.mask) { c.queue = 0; return; }

        const ResistiveMap::Timing& t = _map->timing();
        if (_map->spin() == ResistiveMap::Spin::Hybrid && c.queue >= t.holdAfter) {
            // One long press; the head unit's auto-repeat does the rest.
            // Hold = repeat dead time + one repeat period per owed detent (a
            // fixed hold overshoots: 1.5 s at 500/50 ms = 20 steps for 8 owed).
            // Capped because both constants are Android defaults, not measured.
            uint32_t ms = (uint32_t)t.repeatStartMs + (uint32_t)c.queue * t.repeatMs;
            if (ms > t.holdMaxMs) ms = t.holdMaxMs;
            engage(ch, s.mask, c.queueAction, false, nowMs);
            c.holdEndMs = nowMs + ms;
            c.untilMs   = c.holdEndMs;
            c.queue = 0;
            return;
        }

        engage(ch, s.mask, c.queueAction, false, nowMs);
        --c.queue;
    }

    void ResistiveKeySink::onKey(const Event& e)
    {
        const Explain x = explain(e);
        if (!x.acts) return;

        Channel& c = _ch[x.ch];

        // A learn hold owns the channel; a stray press would corrupt the measurement.
        if (c.learn) return;

        if (x.queued) {
            ++_detents;
            // A reversal empties the queue: the driver already overshot.
            if (c.queue && c.queueAction != e.action) c.queue = 0;
            c.queueAction = e.action;
            if (c.queue >= QUEUE_MAX) { ++_dropped; return; }
            ++c.queue;
            return;
        }

        const ResistiveMap::Slot slot = _map->slot(e.action);

        if (e.state == State::Release) {
            // Deferred short press: released before the long threshold, so
            // send the short rung now as a self-releasing pulse.
            if (c.pending == e.action) {
                c.pending = Action::None;
                engage(x.ch, slot.mask, e.action, false, e.tMs);
                c.untilMs = e.tMs + _map->timing().pulseMs;
                return;
            }
            // Only the owner may release; a preempted button must not open the preemptor's rung.
            if (c.owner == e.action && c.held) open(x.ch, e.tMs);
            return;
        }

        // Long threshold passed while down: engage the long rung, held.
        if (e.state == State::Long && slot.longMask && c.pending == e.action) {
            c.pending = Action::None;
            engage(x.ch, slot.longMask, e.action, true, e.tMs);
            ++_longs;
            return;
        }
        if (e.state != State::Press) return;

        // A new press retires another button's pending decision, or that
        // button's Release would fire its short rung over this one's.
        if (c.pending != Action::None && c.pending != e.action) {
            c.pending = Action::None;
            ++_preempts;
        }

        // A long rung defers the press until short/long is known; this costs
        // the long threshold in latency, so keep it off VOLUME.
        if (slot.longMask) {
            if (c.queue) { c.queue = 0; ++_preempts; }
            if (c.mask && !c.learn) open(x.ch, e.tMs);
            c.pending = e.action;
            return;
        }

        // A button preempts the wheel's queue.
        if (c.queue || (c.mask && !c.held)) { c.queue = 0; ++_preempts; }
        if (c.mask) open(x.ch, e.tMs);
        // Engage now, skipping the gap: a different rung is already a different voltage.
        engage(x.ch, x.mask, e.action, true, e.tMs);
    }

    void ResistiveKeySink::tick(uint32_t nowMs)
    {
        if (!_map) return;

        for (uint8_t i = 0; i < ResistiveMap::CHANNELS; ++i) {
            Channel& c = _ch[i];

            if (c.mask && (int32_t)(nowMs - c.untilMs) >= 0) {
                if (c.learn) {
                    // End of a learn hold: the clean open the study page needs; not "stuck".
                    c.learn = false;
                    open(i, nowMs);
                    continue;
                }
                if (c.held) {
                    // Stuck-key guard fired: a Release went missing upstream.
                    ++_stuck;
                }
                open(i, nowMs);
                continue;
            }

            if (!c.mask && c.queue && (int32_t)(nowMs - c.freeAtMs) >= 0)
                startNextDetent(i, nowMs);
        }
    }
}
