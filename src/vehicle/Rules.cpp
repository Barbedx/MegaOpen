#include "Rules.h"
#include "../utils/PsHeap.h"
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ExprState.h"
#ifndef NATIVE
#include <esp_heap_caps.h>
#endif

namespace Vehicle
{
    namespace
    {
        bool canHold(Keys::Action a) { return !Keys::isEncoder(a) && !Keys::isEncoderPhase(a); }

        bool fail(RuleProblem& p, uint16_t row, const char* code, uint16_t pos = 0)
        {
            p.row = row;
            p.e.code = code;
            p.e.pos = pos;
            return false;
        }

        // A signal's own integer with its decimal point placed: 1180, 2 -> "11.8"
        // (trailing zeros dropped), as a formula reads it.
        int numText(char* b, size_t cap, int32_t v, uint8_t dec)
        {
            if (!dec) return snprintf(b, cap, "%ld", (long)v);
            int32_t p = 1;
            for (uint8_t i = 0; i < dec; ++i) p *= 10;
            const uint32_t a = v < 0 ? (uint32_t)-(int64_t)v : (uint32_t)v;
            int n = snprintf(b, cap, "%s%lu.%0*lu", v < 0 ? "-" : "", (unsigned long)(a / p), (int)dec, (unsigned long)(a % p));
            while (n > 0 && b[n - 1] == '0') b[--n] = 0;
            if (n > 0 && b[n - 1] == '.') b[--n] = 0;
            return n;
        }

        // What 1.6's alerts said on the glass for the two reminders; the rest
        // have the default text (the signal, its value and unit).
        const char* reminderText(uint8_t sig)
        {
            if (sig == SIG_DRIVE_NO_LIGHTS) return "Lights off!";
            if (sig == SIG_DRIVE_HANDBRAKE) return "Handbrake on!";
            return "";
        }

        void putAlert(RuleSet& set, uint8_t sig, uint16_t pauseS, bool on)
        {
            RuleRow r;
            snprintf(r.name, sizeof(r.name), "%s", meta(sig).name);
            r.on = on;
            snprintf(r.when, sizeof(r.when), "rises(%s)", meta(sig).name);
            r.pauseS = pauseS;
            r.act[0].effect = Keys::Effect::Popup;
            snprintf(r.act[0].arg, sizeof(r.act[0].arg), "%s", reminderText(sig));
            r.nAct = 1;
            set.rows.push(r);
        }

        // The first identifier in a formula, for a default popup's subject.
        const char* firstName(const char* s, size_t& len)
        {
            while (*s && !((*s >= 'a' && *s <= 'z') || *s == '_')) ++s;
            const char* b = s;
            while ((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_') ++s;
            len = (size_t)(s - b);
            return b;
        }

        // The subject of a default popup: the formula's first name when it is
        // one of its inputs (not a function's), else the lowest input.
        uint8_t subject(const RuleRow& r, const Expr::SigSet& inputs)
        {
            size_t nl = 0;
            const char* fn = firstName(r.when, nl);
            uint8_t s = SIG_COUNT;
            if (nl && nl < RULE_NAME + 1) {
                char name[RULE_NAME + 1];
                memcpy(name, fn, nl);
                name[nl] = 0;
                s = sigByName(name);
                if (s >= SIG_COUNT || !inputs.has(s)) s = SIG_COUNT;
            }
            for (uint8_t k = 0; s >= SIG_COUNT && k < SIG_COUNT; ++k) if (inputs.has(k)) s = k;
            return s;
        }

        void* bigAlloc(size_t n)
        {
            return PsHeap::malloc(n);
        }
        void bigFree(void* p)
        {
#ifdef NATIVE
            free(p);
#else
            heap_caps_free(p);
#endif
        }
    }

    // ---- the rows -----------------------------------------------------------------

    bool RuleAction::valid() const
    {
        switch (effect) {
            case Keys::Effect::None:    return false;
            case Keys::Effect::Action:  return action != Keys::Action::None;
            case Keys::Effect::Raw:
            case Keys::Effect::KeyCode: return code != 0;
            case Keys::Effect::Text:
            case Keys::Effect::Pane:    return arg[0] != 0;
            case Keys::Effect::Popup:   return true;    // "" = the default text
            default:                    return true;    // Message: any index
        }
    }

    bool RuleGesture::valid() const
    {
        switch (trigger) {
            case Keys::Trigger::Combo: {
                if (members < 2 || members > Keys::Macro::MAX_MEMBERS) return false;
                for (uint8_t i = 0; i < members; ++i) {
                    if (keys[i] == Keys::Action::None || !canHold(keys[i])) return false;
                    for (uint8_t j = 0; j < i; ++j) if (keys[j] == keys[i]) return false;
                }
                return true;
            }
            case Keys::Trigger::Sequence:
                return keys[0] != Keys::Action::None && taps >= 2 && taps <= 10 && windowMs >= 50;
            case Keys::Trigger::Hold:
                return keys[0] != Keys::Action::None && canHold(keys[0]);
            default:
                return false;
        }
    }

    void RuleSet::factory()
    {
        rows.clear();
        putAlert(*this, SIG_FUEL_LOW, 300, true);
        putAlert(*this, SIG_DRIVE_NO_LIGHTS, 60, true);
        putAlert(*this, SIG_DRIVE_HANDBRAKE, 30, true);
    }

    // ---- the engine ---------------------------------------------------------------

    Rules::~Rules()
    {
        if (!_rows) return;
        for (uint16_t i = 0; i < RuleSet::MAX_ROWS; ++i) _rows[i].~Row();
        bigFree(_rows);
        _rows = nullptr;
    }

    bool Rules::rowsReady()
    {
        if (_rows) return true;
        void* m = bigAlloc(sizeof(Row) * RuleSet::MAX_ROWS);
        if (!m) return false;
        _rows = static_cast<Row*>(m);
        for (uint16_t i = 0; i < RuleSet::MAX_ROWS; ++i) new (&_rows[i]) Row();
        return true;
    }

    bool Rules::check(const RuleSet& set, RuleProblem& p)
    {
        p = RuleProblem{};
        if (set.rows.size() > RuleSet::MAX_ROWS) return fail(p, RuleSet::MAX_ROWS, "rule_rows");
        Expr::Program prog;
        for (uint16_t i = 0; i < set.rows.size(); ++i) {
            const RuleRow& r = set.rows[i];
            const size_t nl = strnlen(r.name, sizeof(r.name));
            if (!nl || nl > RULE_NAME) return fail(p, i, "rule_name");
            const bool g = r.isGesture();
            if (g && !r.gesture.valid()) return fail(p, i, "rule_gesture");
            if (!g && !r.when[0]) return fail(p, i, "rule_when");
            if (!g && r.pauseS < RuleRow::PAUSE_MIN_S) return fail(p, i, "rule_pause");
            if (!r.nAct || r.nAct > RuleRow::MAX_ACTIONS) return fail(p, i, "rule_action");
            for (uint8_t k = 0; k < r.nAct; ++k)
                if (!r.act[k].valid()) return fail(p, i, "rule_action");
            if (r.when[0]) {
                Expr::Error e;
                if (!prog.compile(r.when, exprResolve, nullptr, e)) { p.row = i; p.e = e; return false; }
            }
        }
        return true;
    }

    bool Rules::apply(const RuleSet& set, RuleProblem& p)
    {
        p = RuleProblem{};
        const bool ok = check(set, p);
        if (!rowsReady()) return fail(p, 0, "no_memory");
        // Rows past the new set are off before the count moves, and every row
        // starts afresh: counters are the row's, not the rule's.
        for (uint16_t i = 0; i < RuleSet::MAX_ROWS; ++i) _rows[i].on = false;
        _n = set.rows.size() < RuleSet::MAX_ROWS ? set.rows.size() : RuleSet::MAX_ROWS;
        for (uint16_t i = 0; i < _n; ++i) {
            const RuleRow& s = set.rows[i];
            Row& r = _rows[i];
            r.prog.clear();
            r.gesture = s.isGesture();
            r.pauseS = s.pauseS;
            r.seen = false;
            r.fires = 0;
            r.lastFireMs = 0;
            r.holds = !s.when[0];           // a formula-less gesture is always armed
            bool compiled = true;
            if (s.when[0]) {
                Expr::Error e;
                compiled = r.prog.compile(s.when, exprResolve, nullptr, e) && r.prog.reset(r.st);
                if (!compiled) r.prog.clear();
            }
            // The same per-row rules as check(), without the whole-set caps:
            // a row that does not check stays off, the others run.
            bool on = s.on && compiled && (s.isGesture() ? s.gesture.valid() : (s.when[0] && s.pauseS >= RuleRow::PAUSE_MIN_S)) &&
                      s.nAct && s.nAct <= RuleRow::MAX_ACTIONS;
            for (uint8_t k = 0; on && k < s.nAct; ++k) on = s.act[k].valid();
            r.on = on;
        }
        return ok;
    }

    void Rules::step(const State& st, const Expr::SigSet& changed, uint32_t now, bool tick, FireFn fire, void* ctx)
    {
        if (!_rows) return;
        ExprCtx c{&st, now};
        for (uint16_t i = 0; i < _n; ++i) {
            Row& r = _rows[i];
            if (!r.on || r.prog.empty()) continue;
            if (r.seen && !r.prog.inputs().any(changed) && !(tick && r.prog.timed())) continue;
            const bool was = r.holds;
            r.holds = Expr::holds(r.prog.eval(r.st, exprRead, &c, now));
            r.seen = true;
            if (r.gesture || !r.holds || was) continue;
            // false -> true (or true when first seen): fire, pause allowing.
            if (r.fires && (uint32_t)(now - r.lastFireMs) < (uint32_t)r.pauseS * 1000u) continue;
            ++r.fires;
            r.lastFireMs = now;
            if (fire) fire(i, ctx);
        }
    }

    bool Rules::gestureFire(uint16_t row, uint32_t now)
    {
        if (!_rows || row >= _n) return false;
        Row& r = _rows[row];
        if (!r.on || !r.gesture || !r.holds) return false;
        if (r.fires && r.pauseS && (uint32_t)(now - r.lastFireMs) < (uint32_t)r.pauseS * 1000u) return false;
        ++r.fires;
        r.lastFireMs = now;
        return true;
    }

    bool Rules::holds(uint16_t row) const
    {
        return _rows && row < _n && _rows[row].on && _rows[row].holds;
    }

    const Expr::SigSet& Rules::inputsOf(uint16_t row) const
    {
        return _rows && row < _n ? _rows[row].prog.inputs() : _none;
    }

    bool Rules::live(uint16_t row, Live& out) const
    {
        if (!_rows || row >= _n) return false;
        const Row& r = _rows[row];
        out.on = r.on;
        out.gesture = r.gesture;
        out.holds = r.on && r.holds;
        out.running = r.on && (r.gesture || !r.prog.empty());
        out.fires = r.fires;
        out.lastFireMs = r.lastFireMs;
        return true;
    }

    int Rules::popupText(char* b, size_t cap, const RuleRow& r, const char* arg, const Expr::SigSet& inputs, const State& st)
    {
        if (!cap) return 0;
        char num[16];
        if (arg && arg[0]) {
            // {name} -> the value; an unknown name or no value -> "--".
            size_t n = 0;
            for (const char* p = arg; *p && n + 1 < cap;) {
                if (*p != '{') { b[n++] = *p++; continue; }
                const char* e = strchr(p + 1, '}');
                if (!e) { b[n++] = *p++; continue; }
                char name[RULE_NAME + 1];
                const size_t nl = (size_t)(e - p - 1);
                if (nl >= sizeof(name)) { b[n++] = *p++; continue; }
                memcpy(name, p + 1, nl);
                name[nl] = 0;
                const uint8_t s = sigByName(name);
                const char* v = "--";
                if (s < SIG_COUNT && st.has(s)) { numText(num, sizeof(num), st.get(s), meta(s).decimals); v = num; }
                for (const char* q = v; *q && n + 1 < cap; ++q) b[n++] = *q;
                p = e + 1;
            }
            b[n] = 0;
            return (int)n;
        }
        // The default: the formula's first input, as 1.6's alerts said it.
        const uint8_t s = subject(r, inputs);
        if (s >= SIG_COUNT) return snprintf(b, cap, "%s", r.name);
        const Meta& m = meta(s);
        if (!st.has(s)) return snprintf(b, cap, "%s: --", m.name);
        numText(num, sizeof(num), st.get(s), m.decimals);
        const bool unit = m.unit[0] && strcmp(m.unit, "raw") && !(m.unit[0] >= '0' && m.unit[0] <= '9') && m.unit[0] != '-';
        return snprintf(b, cap, "%s: %s%s%s%s", m.name, num, unit ? " " : "", unit ? m.unit : "", verified(s) ? "" : " ?");
    }

    int32_t Rules::popupValue(const RuleRow& r, const Expr::SigSet& inputs, const State& st)
    {
        const uint8_t s = subject(r, inputs);
        return s < SIG_COUNT && st.has(s) ? st.get(s) : 0;
    }

    uint8_t Rules::gestures(const RuleSet& set, Keys::MacroTable& out, uint8_t cap)
    {
        out.clear();
        uint8_t n = 0;
        for (uint16_t i = 0; i < set.rows.size() && n < cap && n < Keys::MacroTable::COUNT; ++i) {
            const RuleRow& r = set.rows[i];
            if (!r.isGesture()) continue;
            Keys::Macro& m = out.m[n];
            m.enabled  = r.on && r.gesture.valid() && r.nAct;
            m.trigger  = r.gesture.trigger;
            for (uint8_t k = 0; k < Keys::Macro::MAX_MEMBERS; ++k) m.keys[k] = r.gesture.keys[k];
            m.members  = r.gesture.members;
            m.taps     = r.gesture.taps;
            m.windowMs = r.gesture.windowMs;
            m.swallow  = r.gesture.swallow;
            if (r.nAct) {
                const RuleAction& a = r.act[0];
                m.effect = a.effect;
                m.action = a.action;
                m.code   = a.code;
                m.dataLen = a.dataLen;
                memcpy(m.data, a.data, sizeof(m.data));
                strncpy(m.arg, a.arg, sizeof(m.arg) - 1);
            }
            strncpy(m.name, r.name, sizeof(m.name) - 1);
            m.rule = i;
            ++n;
        }
        return n;
    }
}
