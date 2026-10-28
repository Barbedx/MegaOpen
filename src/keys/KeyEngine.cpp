#include "KeyEngine.h"
#include "../utils/Log.h"

namespace Keys
{
    void KeyEngine::begin(Input::MatrixScanner* scanner, KeyMap* map)
    {
        _scanner = scanner;
        _map = map;
        applyMap();
    }

    bool KeyEngine::addSink(ISink* s)
    {
        if (!s) return false;
        if (_sinkCount >= MAX_SINKS) {
            LOGW("key", "sink chain full (%u): '%s' gets no keys - raise MAX_SINKS",
                 (unsigned)MAX_SINKS, s->id());
            return false;
        }
        _sinks[_sinkCount++] = s;
        return true;
    }

    void KeyEngine::applyMap()
    {
        if (!_map) return;
        uint8_t a, b, c;
        _map->phases(a, b, c);
        _enc.setPhases(a, b, c);
        _enc.reset();
        _deb.configure(_deb.params(), _map->fastMask());
    }

    void KeyEngine::setTiming(const Input::KeyDebouncer::Params& p)
    {
        _deb.configure(p, _map ? _map->fastMask() : 0);
    }

    void KeyEngine::dispatch(const Event& e)
    {
        // BEFORE the event counter and before the log tap, because a consumed
        // event did not happen as far as the rest of the system is concerned —
        // and a WS line for a press no sink ever saw is exactly the kind of
        // evidence that sends somebody looking at the head unit.
        if (_filter && _filter(e, _filterCtx)) {
            _filtered.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        dispatchRaw(e);
    }

    void KeyEngine::dispatchRaw(const Event& e)
    {
        _events.fetch_add(1, std::memory_order_relaxed);
        for (uint8_t i = 0; i < _sinkCount; ++i) _sinks[i]->onKey(e);
        // After the sinks: the hook formats a log line, which should not sit
        // in front of the key's frame.
        if (_evCb) _evCb(e, _evCtx);
    }

    void KeyEngine::poll(uint32_t nowMs)
    {
        if (!_scanner || !_map) return;

        const uint16_t raw = _scanner->scan();
        Input::PosEvent evs[Input::KeyDebouncer::MAX_EVENTS];
        const uint8_t n = _deb.update(raw, nowMs, evs, Input::KeyDebouncer::MAX_EVENTS);

        for (uint8_t i = 0; i < n; ++i) {
            const Input::PosEvent& pe = evs[i];
            const Slot& slot = _map->slot(pe.pos);

            if (isEncoderPhase(slot.action)) {
                if (pe.edge != Input::Edge::Down) continue;   // phases pulse, they do not hold
                const int8_t step = _enc.onDown(pe.pos);
                if (!step) continue;
                Event e;
                e.action   = (step > 0) ? Action::EncCw : Action::EncCcw;
                e.state    = State::Press;
                e.tMs      = pe.tMs;
                e.srcId    = 1;
                e.codeHint = _map->encCode(step > 0);
                deliver(e);
                continue;
            }

            if (slot.action == Action::None) continue;        // unassigned position

            Event e;
            e.action = slot.action;
            e.tMs    = pe.tMs;
            e.srcId  = 0;
            e.codeHint = slot.code;
            switch (pe.edge) {
                case Input::Edge::Down:   e.state = State::Press;   break;
                case Input::Edge::Up:     e.state = State::Release; break;
                case Input::Edge::Long:   e.state = State::Long;    break;
                case Input::Edge::Repeat: e.state = State::Repeat;  break;
            }
            deliver(e);
        }
    }
}
