#include "MacroEngine.h"
#include <string.h>

namespace Keys
{
    // A wheel detent is a Press with no Release, so it can never be held: it may
    // not be a combo member, and must never be muted (no Release would clear it).
    static inline bool canHold(Action a) { return !isEncoder(a) && !isEncoderPhase(a); }

    static inline uint32_t bitOf(Action a) { return 1u << (uint8_t)a; }

    // Row i is the enum value i. `taps`: the trigger counts presses; `field`:
    // the one Macro member the effect reads ("" none) — the page shows only that.
    const TriggerRow TRIGGERS[] = {
        {"off",      "вимк",      false},
        {"combo",    "разом",     false},
        {"sequence", "натиски",   true},
        {"hold",     "утримання", false},
    };
    const EffectRow EFFECTS[] = {
        {"none",    "нічого",       ""},
        {"action",  "дія",          "action"},
        {"key",     "код у ГУ",     "code"},
        {"raw",     "кадр Raise",   "raw"},
        {"text",    "текст",        "text"},
        {"popup",   "попап",        "text"},
        {"pane",    "сцена",        "scene"},
        {"message", "повідомлення", "code"},
    };

    const char* triggerName(Trigger t)
    {
        const uint8_t i = (uint8_t)t;
        return TRIGGERS[i < (uint8_t)Trigger::_Count ? i : 0].id;
    }

    Trigger triggerFromName(const char* s)
    {
        if (!s) return Trigger::Off;
        for (uint8_t i = 0; i < (uint8_t)Trigger::_Count; ++i)
            if (!strcmp(s, TRIGGERS[i].id)) return (Trigger)i;
        return Trigger::Off;
    }

    const char* effectName(Effect e)
    {
        const uint8_t i = (uint8_t)e;
        return EFFECTS[i < (uint8_t)Effect::_Count ? i : 0].id;
    }

    Effect effectFromName(const char* s)
    {
        if (!s) return Effect::None;
        for (uint8_t i = 0; i < (uint8_t)Effect::_Count; ++i)
            if (!strcmp(s, EFFECTS[i].id)) return (Effect)i;
        return Effect::None;
    }

    EffectRoute effectRoute(Effect e)
    {
        switch (e) {
            case Effect::Action:
            case Effect::KeyCode: return EffectRoute::Keys;
            case Effect::Raw:     return EffectRoute::UartOut;
            case Effect::Message: return EffectRoute::Hu;
            case Effect::Text:
            case Effect::Popup:   return EffectRoute::Panel;
            case Effect::Pane:    return EffectRoute::Pane;
            default:              return EffectRoute::None;
        }
    }

    // Whether a row can ever fire; an invalid macro never matches. Shown per row
    // in the UI.
    bool Macro::valid() const
    {
        if (effect == Effect::None) return false;
        switch (effect) {
            case Effect::Action:  if (action == Action::None) return false; break;
            case Effect::Raw:     if (!code) return false; break;
            case Effect::KeyCode: if (!code) return false; break;
            case Effect::Text:
            case Effect::Popup:
            case Effect::Pane:    if (!arg[0]) return false; break;
            default: break;
        }
        switch (trigger) {
            case Trigger::Combo: {
                if (members < 2 || members > MAX_MEMBERS) return false;
                for (uint8_t i = 0; i < members; ++i) {
                    if (keys[i] == Action::None || !canHold(keys[i])) return false;
                    for (uint8_t j = 0; j < i; ++j) if (keys[j] == keys[i]) return false;
                }
                return true;
            }
            case Trigger::Sequence:
                return keys[0] != Action::None && taps >= 2 && taps <= 10 && windowMs >= 50;
            case Trigger::Hold:
                return keys[0] != Action::None && canHold(keys[0]);
            default:
                return false;
        }
    }

    void MacroEngine::clear()
    {
        for (uint8_t i = 0; i < MAX_MACROS; ++i) { _m[i] = Macro{}; _r[i] = Run{}; }
        reset();
    }

    void MacroEngine::reset()
    {
        _pendN = 0; _pendMacro = -1; _pendDeadline = 0;
        _down = 0; _muted = 0;
        for (uint8_t i = 0; i < MAX_MACROS; ++i) { _r[i].taps = 0; _r[i].engaged = false; }
    }

    // Two examples, both disabled.
    void MacroTable::loadDefaults()
    {
        clear();

        Macro& a = m[0];
        a.trigger = Trigger::Combo;
        a.keys[0] = Action::VolUp; a.keys[1] = Action::VolDown; a.members = 2;
        a.effect  = Effect::KeyCode;
        a.code    = 0x07;            // SOURCE — long-SOURCE is mute on this profile
        a.swallow = true;
        strncpy(a.name, "VOL+ & VOL-", Macro::NAME_LEN - 1);

        Macro& b = m[1];
        b.trigger  = Trigger::Sequence;
        b.keys[0]  = Action::Mute;
        b.taps     = 3;
        b.windowMs = 500;
        b.swallow  = true;
        b.effect   = Effect::Popup;
        strncpy(b.arg, "MACRO", Macro::ARG_LEN - 1);
        strncpy(b.name, "MUTE x3", Macro::NAME_LEN - 1);
    }

    void MacroEngine::loadDefaults()
    {
        MacroTable t;
        t.loadDefaults();
        clear();
        for (uint8_t i = 0; i < MAX_MACROS; ++i) _m[i] = t.m[i];
    }

    void MacroEngine::setTable(const MacroTable& t)
    {
        for (uint8_t i = 0; i < MAX_MACROS; ++i) {
            const uint32_t fires = _m[i].fires;
            const uint32_t last  = _m[i].lastMs;
            _m[i] = t.m[i];
            _m[i].fires  = fires;
            _m[i].lastMs = last;
        }
        // Drop half-collected sequences and mutes from the old table, or a key
        // muted by a deleted macro stays muted until released.
        reset();
    }

    void MacroEngine::table(MacroTable& out) const
    {
        for (uint8_t i = 0; i < MAX_MACROS; ++i) out.m[i] = _m[i];
    }

    int8_t MacroEngine::sequenceFor(Action a) const
    {
        for (uint8_t i = 0; i < MAX_MACROS; ++i)
            if (_m[i].enabled && _m[i].trigger == Trigger::Sequence &&
                _m[i].keys[0] == a && _m[i].taps >= 2)
                return (int8_t)i;
        return -1;
    }

    void MacroEngine::push(const Event& e)
    {
        if (_pendN < PEND_MAX) { _pend[_pendN++] = e; ++_swallowed; return; }
        // Buffer full: replay everything rather than drop a press.
        flush();
        if (_emit) _emit(e, _emitCtx);
        ++_replayed;
    }

    void MacroEngine::flush()
    {
        for (uint8_t i = 0; i < _pendN; ++i) {
            if (_emit) _emit(_pend[i], _emitCtx);
            ++_replayed;
        }
        _pendN = 0;
        if (_pendMacro >= 0) _r[_pendMacro].taps = 0;
        _pendMacro = -1;
    }

    void MacroEngine::fire(uint8_t i, const Event& e)
    {
        Macro& m = _m[i];
        ++m.fires;
        m.lastMs = e.tMs;
        ++_fired;
        if (_fire) _fire(m, _fireCtx);
    }

    void MacroEngine::run(uint8_t i)
    {
        if (i >= MAX_MACROS || !_fire) return;
        // Not fire(): a test run leaves the counters alone.
        _fire(_m[i], _fireCtx);
    }

    uint8_t MacroEngine::progress(uint8_t i) const
    {
        return i < MAX_MACROS ? _r[i].taps : 0;
    }

    bool MacroEngine::matchCombo(const Event& e)
    {
        if (e.state != State::Press) return false;

        for (uint8_t i = 0; i < MAX_MACROS; ++i) {
            const Macro& m = _m[i];
            if (!m.enabled || m.trigger != Trigger::Combo || !m.valid()) continue;
            if (_r[i].engaged) continue;

            // The new key must be a member, or a held combo re-fires on every
            // unrelated press.
            uint32_t need = 0;
            bool member = false;
            for (uint8_t j = 0; j < m.members; ++j) {
                need |= bitOf(m.keys[j]);
                if (m.keys[j] == e.action) member = true;
            }
            if (!member || (_down & need) != need) continue;

            _r[i].engaged = true;

            if (m.swallow) {
                // Members already held were dispatched and need a Release; the
                // new key's Press is being consumed, so it gets none.
                for (uint8_t j = 0; j < m.members; ++j) {
                    const Action a = m.keys[j];
                    if (a == e.action || !(_down & bitOf(a))) continue;
                    Event r = e;
                    r.action = a;
                    r.state  = State::Release;
                    r.codeHint = 0;
                    if (_emit) _emit(r, _emitCtx);
                    _muted |= bitOf(a);
                }
                _muted |= bitOf(e.action);
                ++_swallowed;
            }

            fire(i, e);
            return m.swallow;
        }
        return false;
    }

    bool MacroEngine::matchHold(const Event& e)
    {
        if (e.state != State::Long) return false;

        for (uint8_t i = 0; i < MAX_MACROS; ++i) {
            const Macro& m = _m[i];
            if (!m.enabled || m.trigger != Trigger::Hold || !m.valid()) continue;
            if (m.keys[0] != e.action || _r[i].engaged) continue;

            _r[i].engaged = true;

            if (m.swallow) {
                // The Press already went out; only Long/Repeat/Release can be stopped.
                Event r = e;
                r.state = State::Release;
                if (_emit) _emit(r, _emitCtx);
                _muted |= bitOf(e.action);
                ++_swallowed;
            }

            fire(i, e);
            return m.swallow;
        }
        return false;
    }

    bool MacroEngine::matchSequence(const Event& e)
    {
        const int8_t idx = sequenceFor(e.action);
        if (idx < 0) return false;
        const Macro& m = _m[idx];

        // Long/Repeat are not taps: replay what was collected, in order.
        if (e.state == State::Long || e.state == State::Repeat) {
            if (_pendMacro == idx) { push(e); flush(); return true; }
            _r[idx].taps = 0;
            return false;
        }

        if (e.state == State::Press) {
            // A different sequence was mid-flight; replay it first.
            if (_pendMacro >= 0 && _pendMacro != idx) flush();

            Run& r = _r[idx];
            // Also checked here in case tick() did not run.
            if (r.taps && (uint32_t)(e.tMs - r.lastMs) > m.windowMs) {
                if (_pendMacro == idx) flush();
                r.taps = 0;
            }
            ++r.taps;
            r.lastMs = e.tMs;

            if (r.taps >= m.taps) {
                r.taps = 0;
                if (_pendMacro == idx) {         // the earlier taps never happened
                    _pendN = 0;
                    _pendMacro = -1;
                }
                fire(idx, e);
                if (m.swallow) {
                    // Consume this press and its release.
                    if (canHold(e.action)) _muted |= bitOf(e.action);
                    ++_swallowed;
                    return true;
                }
                return false;
            }

            if (m.swallow) {
                _pendMacro = idx;
                _pendDeadline = e.tMs + m.windowMs;
                push(e);
                return true;
            }
            return false;
        }

        // Release of a held-back tap is held too.
        if (m.swallow && _pendMacro == idx) { push(e); return true; }
        return false;
    }

    bool MacroEngine::observe(const Event& e)
    {
        if (e.action == Action::None || isEncoderPhase(e.action)) return false;

        const uint32_t bit = bitOf(e.action);

        // Only holdable keys are tracked; a detent would leave its bit set forever.
        if (canHold(e.action)) {
            if      (e.state == State::Press)   _down |= bit;
            else if (e.state == State::Release) _down &= ~bit;
        }

        // Re-arm before the mute check: a swallowed member's Release is consumed
        // below and would never reach matchCombo.
        if (e.state == State::Release) {
            for (uint8_t i = 0; i < MAX_MACROS; ++i) {
                const Macro& m = _m[i];
                if (m.trigger == Trigger::Combo) {
                    for (uint8_t j = 0; j < m.members; ++j)
                        if (m.keys[j] == e.action) { _r[i].engaged = false; break; }
                } else if (m.trigger == Trigger::Hold && m.keys[0] == e.action) {
                    _r[i].engaged = false;
                }
            }
        }

        // Any other key breaks a sequence in flight; flush now so held taps go
        // out before the interrupting key, not after.
        if (_pendN && _pendMacro >= 0 && _m[_pendMacro].keys[0] != e.action) flush();

        bool consumed;
        if (_muted & bit) {
            if (e.state == State::Release) _muted &= ~bit;
            ++_swallowed;
            consumed = true;
        } else if (matchCombo(e) || matchHold(e)) {
            consumed = true;
        } else {
            consumed = matchSequence(e);
        }
        return consumed;
    }

    void MacroEngine::tick(uint32_t nowMs)
    {
        if (_pendN && (int32_t)(nowMs - _pendDeadline) >= 0) flush();
    }

    // ---- persistence --------------------------------------------------------
    static constexpr uint8_t kBlobVersion = 1;

    void MacroEngine::toBlob(uint8_t* out) const
    {
        MacroTable t;
        table(t);
        toBlob(t, out);
    }

    bool MacroEngine::fromBlob(const uint8_t* in, uint16_t len)
    {
        MacroTable t;
        if (!fromBlob(t, in, len)) return false;
        for (uint8_t i = 0; i < MAX_MACROS; ++i) _m[i] = t.m[i];
        reset();
        return true;
    }

    void MacroEngine::toBlob(const MacroTable& t, uint8_t* out)
    {
        uint16_t p = 0;
        out[p++] = kBlobVersion;
        for (uint8_t i = 0; i < MAX_MACROS; ++i) {
            const Macro& m = t.m[i];
            out[p++] = m.enabled ? 1 : 0;
            out[p++] = (uint8_t)m.trigger;
            for (uint8_t j = 0; j < Macro::MAX_MEMBERS; ++j) out[p++] = (uint8_t)m.keys[j];
            out[p++] = m.members;
            out[p++] = m.taps;
            out[p++] = (uint8_t)(m.windowMs & 0xFF);
            out[p++] = (uint8_t)(m.windowMs >> 8);
            out[p++] = m.swallow ? 1 : 0;
            out[p++] = (uint8_t)m.effect;
            out[p++] = (uint8_t)m.action;
            out[p++] = m.code;
            out[p++] = m.dataLen;
            for (uint8_t j = 0; j < Macro::MAX_DATA; ++j) out[p++] = m.data[j];
            for (uint8_t j = 0; j < Macro::ARG_LEN;  ++j) out[p++] = (uint8_t)m.arg[j];
            for (uint8_t j = 0; j < Macro::NAME_LEN; ++j) out[p++] = (uint8_t)m.name[j];
        }
    }

    bool MacroEngine::fromBlob(MacroTable& t, const uint8_t* in, uint16_t len)
    {
        if (!in || len != BLOB_SIZE || in[0] != kBlobVersion) return false;

        uint16_t p = 1;
        for (uint8_t i = 0; i < MAX_MACROS; ++i) {
            Macro m;
            m.enabled = in[p++] != 0;
            const uint8_t tr = in[p++];
            m.trigger = tr < (uint8_t)Trigger::_Count ? (Trigger)tr : Trigger::Off;
            for (uint8_t j = 0; j < Macro::MAX_MEMBERS; ++j) {
                const uint8_t a = in[p++];
                m.keys[j] = a < (uint8_t)Action::_Count ? (Action)a : Action::None;
            }
            m.members = in[p] <= Macro::MAX_MEMBERS ? in[p] : Macro::MAX_MEMBERS; ++p;
            m.taps = in[p++];
            if (m.taps < 2)  m.taps = 2;
            if (m.taps > 10) m.taps = 10;
            m.windowMs = (uint16_t)(in[p] | ((uint16_t)in[p + 1] << 8)); p += 2;
            if (m.windowMs < 50) m.windowMs = 50;
            m.swallow = in[p++] != 0;
            const uint8_t ef = in[p++];
            m.effect = ef < (uint8_t)Effect::_Count ? (Effect)ef : Effect::None;
            const uint8_t ac = in[p++];
            m.action = ac < (uint8_t)Action::_Count ? (Action)ac : Action::None;
            m.code = in[p++];
            m.dataLen = in[p] <= Macro::MAX_DATA ? in[p] : Macro::MAX_DATA; ++p;
            for (uint8_t j = 0; j < Macro::MAX_DATA; ++j) m.data[j] = in[p++];
            for (uint8_t j = 0; j < Macro::ARG_LEN;  ++j) m.arg[j]  = (char)in[p++];
            for (uint8_t j = 0; j < Macro::NAME_LEN; ++j) m.name[j] = (char)in[p++];
            // Force terminators on stored strings.
            m.arg[Macro::ARG_LEN - 1] = '\0';
            m.name[Macro::NAME_LEN - 1] = '\0';
            t.m[i] = m;
        }
        return true;
    }
}
