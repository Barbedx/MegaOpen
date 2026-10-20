#include "KeyRouter.h"

namespace Keys
{
    const char* laneName(Lane l)
    {
        switch (l) {
            case Lane::Rel: return "rel";
            case Lane::Tap: return "tap";
            case Lane::Web: return "web";
        }
        return "?";
    }

    uint8_t KeyQueues::size(Lane l) const
    {
        switch (l) {
            case Lane::Rel: return _rel.n;
            case Lane::Tap: return _tap.n;
            case Lane::Web: return _web.n;
        }
        return 0;
    }

    bool KeyQueues::push(Lane l, KeyMsg m)
    {
        m.seq = ++_seq;
        switch (l) {
            case Lane::Rel:
                if (_rel.full()) {
                    ++_drops[(uint8_t)Lane::Rel];
                    // Remembered so the keys task can still let the key go.
                    // Past LOST_MAX it cannot be named: every key down goes.
                    if (m.kind == KeyMsg::Kind::Event) {
                        if (_lostN < LOST_MAX) {
                            _lostSeq[_lostN] = m.seq;
                            _lost[_lostN++] = m.ev;
                        } else {
                            ++_lostOverflow;
                            _lostAll = true;
                            _lostAllSeq = m.seq;
                        }
                    }
                    return false;
                }
                _rel.push(m);
                return true;
            case Lane::Tap:
                if (_tap.full()) {
                    ++_drops[(uint8_t)Lane::Tap];
                    // Per origin: a fast encoder spin shows up as its own number.
                    const uint8_t src = m.kind == KeyMsg::Kind::PanelKey ? 3 : m.ev.srcId;
                    if (src < ORIGINS) ++_tapDropsBy[src];
                    return false;
                }
                _tap.push(m);
                return true;
            case Lane::Web:
                if (_web.full()) { ++_drops[(uint8_t)Lane::Web]; return false; }
                _web.push(m);
                return true;
        }
        return false;
    }

    // An encoder detent: a press with no release, which a release may pass.
    // A PanelKey is not known to be one until AffaKeySource maps it, so it
    // counts as a real key and keeps its place.
    static bool isDetent(const KeyMsg& m)
    {
        return (m.kind == KeyMsg::Kind::Event || m.kind == KeyMsg::Kind::Tap) &&
               (isEncoder(m.ev.action) || isEncoderPhase(m.ev.action));
    }

    bool KeyQueues::pop(KeyMsg& out)
    {
        if (_rel.n) {
            const KeyMsg& r = _rel.at(0);
            // A real key's press queued before this release (its own press, or
            // another key's): the Tap lane in arrival order up to it first, so
            // a release never lands ahead of a press that came before it.
            for (uint8_t i = 0; i < _tap.n; ++i) {
                const KeyMsg& t = _tap.at(i);
                if ((int32_t)(t.seq - r.seq) > 0) break;     // queued after the release
                if (!isDetent(t)) { _tap.pop(out); return true; }
            }
            _rel.pop(out);
            return true;
        }
        if (_tap.n) { _tap.pop(out); return true; }
        if (_web.n) { _web.pop(out); return true; }
        return false;
    }

    uint8_t KeyQueues::takeLost(Event* out, uint8_t max)
    {
        uint8_t n = 0;
        for (; n < _lostN && n < max; ++n) out[n] = _lost[n];
        _lostN = 0;
        return n;
    }

    bool KeyQueues::olderGone(uint32_t seq) const
    {
        // Each lane is in arrival order: its head is its oldest item.
        if (_rel.n && (int32_t)(_rel.at(0).seq - seq) < 0) return false;
        if (_tap.n && (int32_t)(_tap.at(0).seq - seq) < 0) return false;
        return true;
    }

    uint8_t KeyQueues::takeLostReady(Event* out, uint8_t max, bool& releaseAll)
    {
        // _lost is in arrival order too, so the ready ones are a prefix.
        uint8_t n = 0;
        while (n < _lostN && n < max && olderGone(_lostSeq[n])) { out[n] = _lost[n]; ++n; }
        for (uint8_t i = n; i < _lostN; ++i) {
            _lost[i - n]    = _lost[i];
            _lostSeq[i - n] = _lostSeq[i];
        }
        _lostN = (uint8_t)(_lostN - n);
        releaseAll = false;
        if (_lostAll && olderGone(_lostAllSeq)) {
            releaseAll = true;
            _lostAll = false;
        }
        return n;
    }

    // ---- DownSet ----------------------------------------------------------------

    bool DownSet::has(uint8_t srcId, Action a) const
    {
        for (uint8_t i = 0; i < _n; ++i)
            if (_k[i].src == srcId && _k[i].a == a) return true;
        return false;
    }

    void DownSet::note(const Event& e)
    {
        // A detent is a press with no release by design; the sink schedules
        // its own. Tracking one would leave it "down" for ever.
        if (e.action == Action::None || isEncoder(e.action) || isEncoderPhase(e.action)) return;

        if (e.state == State::Release) {
            for (uint8_t i = 0; i < _n; ++i) {
                if (_k[i].src == e.srcId && _k[i].a == e.action) {
                    _k[i] = _k[--_n];
                    return;
                }
            }
            return;
        }
        // Press and Long: down. Repeat says nothing new.
        if (e.state != State::Press && e.state != State::Long) return;
        if (has(e.srcId, e.action)) return;
        if (_n >= MAX) { ++_overflow; return; }
        _k[_n++] = Key{e.srcId, e.action, e.codeHint};
    }

    Event DownSet::releaseOf(uint8_t i) const
    {
        Event r;
        if (i >= _n) return r;
        r.action   = _k[i].a;
        r.state    = State::Release;
        r.srcId    = _k[i].src;
        r.codeHint = _k[i].code;
        return r;
    }

    // ---- KeyRouter --------------------------------------------------------------

    void KeyRouter::handle(const KeyMsg& m)
    {
        switch (m.kind) {
            case KeyMsg::Kind::Event:
                if (_h.dispatch) _h.dispatch(m.ev, _h.ctx);
                break;
            case KeyMsg::Kind::Tap: {
                if (!_h.dispatch) break;
                Event e = m.ev;
                if (e.state == State::Release) e.state = State::Press;
                _h.dispatch(e, _h.ctx);
                if (!isEncoder(e.action)) {
                    e.state = State::Release;
                    _h.dispatch(e, _h.ctx);
                }
                break;
            }
            case KeyMsg::Kind::PanelKey:
                if (_h.panelKey) _h.panelKey(m.code, m.a != 0, m.ev.tMs, _h.ctx);
                break;
            case KeyMsg::Kind::Literal:
                if (_h.literal) _h.literal((uint8_t)m.code, m.a, m.b != 0, _h.ctx);
                break;
            case KeyMsg::Kind::MacroRun:
                if (_h.macroRun) _h.macroRun(m.a, _h.ctx);
                break;
        }
    }

    uint8_t KeyRouter::synthesise(const Event* lost, uint8_t n, uint32_t nowMs)
    {
        uint8_t sent = 0;
        for (uint8_t i = 0; i < n; ++i) {
            // Not down: the press was dropped too, or a real release already
            // let it go. Nothing to do either way.
            if (!_down.has(lost[i].srcId, lost[i].action)) continue;
            Event r = lost[i];
            r.state = State::Release;
            r.tMs   = nowMs;
            ++_stuckSynth;
            ++sent;
            if (_h.dispatch) _h.dispatch(r, _h.ctx);
            // dispatch() reaches observe() through the engine's event hook, but
            // a macro may swallow the release; the key is let go either way.
            _down.note(r);
        }
        return sent;
    }

    uint8_t KeyRouter::releaseAll(uint32_t nowMs)
    {
        // A copy first: each dispatch edits the set through observe().
        Event r[DownSet::MAX];
        const uint8_t n = _down.size();
        for (uint8_t i = 0; i < n; ++i) { r[i] = _down.releaseOf(i); r[i].tMs = nowMs; }
        for (uint8_t i = 0; i < n; ++i) {
            ++_stuckSynth;
            if (_h.dispatch) _h.dispatch(r[i], _h.ctx);
            _down.note(r[i]);
        }
        return n;
    }
}
