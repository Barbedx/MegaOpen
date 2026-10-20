#include "AffaKeySource.h"

namespace Keys
{
    void AffaKeySource::dispatch(affa::Key k, bool hold, uint32_t tMs)
    {
        if (!_engine || !_map) return;

        // Filled first so every path can report, including unmapped ones.
        Report rep;
        rep.key   = k;
        rep.hold  = hold;
        rep.known = (AffaMap::indexOf(k) < AffaMap::KEYS);

        AffaMap::Row r;
        if (!_map->lookup(k, hold, r)) {
            // Unknown code or a deliberately emptied row; `known` tells them apart.
            ++_unmapped;
            _lastUnmapped = static_cast<uint16_t>(k);
            _lastUnmappedHold = hold;
            if (_report) _report(rep, _reportCtx);
            return;
        }

        ++_events;

        rep.mapped = true;
        rep.action = r.action;
        rep.code   = r.code;
        rep.asLong = r.asLong;

        Event ev;
        ev.action   = r.action;
        ev.srcId    = 3;                    // 3 = OEM display over CAN
        // The source time: the sink schedules releases against it (see NowFn).
        ev.tMs      = tMs;
        ev.codeHint = r.code;               // 0 = let the sink use its profile default

        // Reported before dispatch; the callback only formats. Kept ahead on
        // purpose: a macro this key completes may post its own popup from
        // inside dispatch, and the key popup must not land on top of it.
        if (_report) _report(rep, _reportCtx);

        // A wheel detent has no release. `hold` is ignored: the map's wheel hold
        // rows are empty by default.
        if (isEncoder(r.action)) {
            ev.state = State::Press;
            _engine->dispatch(ev);
            return;
        }

        // asLong is what the head unit is told, independent of the panel's edge
        // (hold-SRC can map to a plain MUTE press).
        ev.state = r.asLong ? State::Long : State::Press;
        _engine->dispatch(ev);
        ev.state = State::Release;
        _engine->dispatch(ev);
    }

    void AffaKeySource::onPanelKey(uint16_t code, bool hold, uint32_t tMs)
    {
        dispatch(static_cast<affa::Key>(code), hold, tMs);
    }

    void AffaKeySource::handle(affa::Key k, affa::KeyEdge e, void* ctx)
    {
        if (!ctx) return;
        AffaKeySource* self = static_cast<AffaKeySource*>(ctx);
        self->dispatch(k, e == affa::KeyEdge::Hold, self->_now ? self->_now() : 0);
    }
}
