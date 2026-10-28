#include "Formulas.h"
#include <new>
#include <string.h>
#include "ExprState.h"

namespace Vehicle
{
    namespace
    {
        void put(FormulaRow& r, const char* name, const char* src)
        {
            r = FormulaRow{};
            strncpy(r.name, name, USER_NAME);
            strncpy(r.src, src, Expr::Program::MAX_SRC);
        }

        bool validName(const char* n)
        {
            const size_t len = strnlen(n, USER_NAME + 1);
            if (!len || len > USER_NAME) return false;
            if (!((n[0] >= 'a' && n[0] <= 'z') || n[0] == '_')) return false;
            for (size_t i = 1; i < len; ++i)
                if (!((n[i] >= 'a' && n[i] <= 'z') || (n[i] >= '0' && n[i] <= '9') || n[i] == '_')) return false;
            return !Expr::reserved(n, len);
        }

        bool builtin(const char* n, uint8_t& sig)
        {
            sig = sigByName(n);
            return sig < SIG_USER_0;
        }

        // check(): a name is a built-in, or one of the set's new ones (given
        // ids past the user slots' start, in row order).
        struct Names {
            const FormulaSet* set;
        };
        int resolveIn(const char* name, size_t len, void* ctx)
        {
            const FormulaSet& set = *static_cast<Names*>(ctx)->set;
            const int b = exprResolve(name, len, nullptr);
            if (b >= 0 && b < SIG_USER_0) return b;
            uint8_t k = 0;
            for (uint16_t i = 0; i < set.rows.size(); ++i) {
                const FormulaRow& r = set.rows[i];
                uint8_t s;
                if (builtin(r.name, s)) continue;
                if (strlen(r.name) == len && !strncmp(r.name, name, len)) return SIG_USER_0 + k;
                ++k;
            }
            return -1;
        }

        bool fail(FormulaProblem& p, uint16_t row, const char* code, uint16_t pos = 0)
        {
            p.row = row;
            p.e.code = code;
            p.e.pos = pos;
            return false;
        }
    }

    void FormulaSet::factory()
    {
        rows.clear();
        FormulaRow r;
        // The tank in per cent: the Mégane II's is 60 L.
        put(r, "fuel_pct", "fuel_level * 100 / 60");
        rows.push(r);
        // How far what is left goes at the average so far.
        put(r, "range_calc", "fuel_level * 100 / cons_avg");
        rows.push(r);
    }

    bool Formulas::check(const FormulaSet& set, FormulaProblem& p)
    {
        p = FormulaProblem{};
        if (set.rows.size() > FormulaSet::MAX_ROWS) return fail(p, FormulaSet::MAX_ROWS, "formula_rows");
        uint8_t fresh = 0;
        for (uint16_t i = 0; i < set.rows.size(); ++i) {
            const FormulaRow& r = set.rows[i];
            uint8_t s;
            if (!builtin(r.name, s)) {
                if (!validName(r.name)) return fail(p, i, "formula_name");
                if (++fresh > USER_SIGNALS) return fail(p, i, "formula_rows");
            }
            for (uint16_t j = 0; j < i; ++j)
                if (!strcmp(set.rows[j].name, r.name)) return fail(p, i, "formula_twice");
        }
        Names n{&set};
        Expr::Program prog;
        for (uint16_t i = 0; i < set.rows.size(); ++i) {
            Expr::Error e;
            if (!prog.compile(set.rows[i].src, resolveIn, &n, e)) { p.row = i; p.e = e; return false; }
        }
        return true;
    }

    bool Formulas::apply(const FormulaSet& set, FormulaProblem& p)
    {
        p = FormulaProblem{};
        const bool ok = check(set, p);

        // The user slots: a name that had one keeps it, a new one takes the
        // first slot no name of the new set holds. Working space off the
        // stack: this runs on the car task.
        PsArray<Name> want, slot;
        PsArray<uint8_t> placed;
        PsArray<UserDef> defs;
        if (!_slotName.size() && !_slotName.resize(USER_SIGNALS)) return fail(p, 0, "no_memory");
        if (!slot.resize(USER_SIGNALS) || !defs.resize(USER_SIGNALS) || !want.reserve(USER_SIGNALS) ||
            !placed.resize(USER_SIGNALS))
            return fail(p, 0, "no_memory");
        for (uint16_t i = 0; i < set.rows.size() && want.size() < USER_SIGNALS; ++i) {
            uint8_t s;
            const FormulaRow& r = set.rows[i];
            if (builtin(r.name, s) || !validName(r.name)) continue;
            Name n{};
            strncpy(n.n, r.name, USER_NAME);
            want.push(n);
        }
        for (uint16_t k = 0; k < want.size(); ++k)
            for (uint8_t s = 0; s < USER_SIGNALS && !placed[k]; ++s)
                if (_slotName[s].n[0] && !strcmp(_slotName[s].n, want[k].n)) { slot[s] = want[k]; placed[k] = 1; }
        for (uint16_t k = 0; k < want.size(); ++k) {
            if (placed[k]) continue;
            for (uint8_t s = 0; s < USER_SIGNALS; ++s)
                if (!slot[s].n[0]) { slot[s] = want[k]; break; }
        }
        for (uint8_t s = 0; s < USER_SIGNALS; ++s) {
            _slotName[s] = slot[s];
            if (!_slotName[s].n[0]) continue;
            for (uint16_t i = 0; i < set.rows.size(); ++i) {
                const FormulaRow& r = set.rows[i];
                if (strcmp(r.name, _slotName[s].n)) continue;
                defs[s].name = _slotName[s].n;
                defs[s].unit = r.unit;
                defs[s].decimals = r.decimals;
                defs[s].boolean = r.boolean;
                break;
            }
        }
        setUserSignals(defs.data());

        // The rows, compiled against the signals as they are now.
        for (Live*& l : _row) { delete l; l = nullptr; }
        _live.clear();
        _targets.clear();
        for (uint16_t i = 0; i < set.rows.size() && i < FormulaSet::MAX_ROWS; ++i) {
            const FormulaRow& r = set.rows[i];
            if (!r.on) continue;
            const uint8_t t = sigByName(r.name);
            if (t >= SIG_COUNT) continue;
            Live* l = new (std::nothrow) Live();
            if (!l) return fail(p, i, "no_memory");
            Expr::Error e;
            if (!l->prog.compile(r.src, exprResolve, nullptr, e) || !l->prog.reset(l->st)) { delete l; continue; }
            l->target = t;
            _row[i] = l;
            _live.push((uint8_t)i);
            _targets.set(t);
        }
        return ok;
    }

    void Formulas::step(State& st, Expr::SigSet& changed, uint32_t now, bool tick)
    {
        ExprCtx c{&st, now};
        for (uint16_t k = 0; k < _live.size(); ++k) {
            Live& l = *_row[_live[k]];
            if (_bound.has(l.target)) continue;
            if (!l.prog.inputs().any(changed) && !(tick && l.prog.timed())) continue;
            const Expr::Num v = l.prog.eval(l.st, exprRead, &c, now);
            const bool had = st.has(l.target);
            const int32_t was = st.get(l.target);
            int32_t raw;
            if (Expr::toRaw(v, meta(l.target).decimals, raw)) {
                raw = State::clamp(l.target, raw);
                st.set(l.target, raw, now);
                if (!st.pinned(l.target) && (!had || raw != was)) changed.set(l.target);
            } else if (had && !st.pinned(l.target)) {
                st.clear(l.target);
                changed.set(l.target);
            }
        }
    }

    bool Formulas::inputsOf(uint8_t sig, Expr::SigSet& out) const
    {
        for (uint16_t k = 0; k < _live.size(); ++k) {
            const Live& l = *_row[_live[k]];
            if (l.target == sig) { out = l.prog.inputs(); return true; }
        }
        return false;
    }
}
