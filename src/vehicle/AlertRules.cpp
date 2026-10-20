#include "AlertRules.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace Vehicle
{
    // ---- the table -------------------------------------------------------------

    void AlertRules::factory()
    {
        for (AlertRule& x : r) x = AlertRule{};
        r[0].sig = SIG_FUEL_LOW;
        r[0].cond = AC_RISES;
        r[0].cooldownS = 300;
        r[0].on = true;
    }

    int32_t AlertRules::get(uint8_t row, uint8_t col) const
    {
        if (row >= ROWS) return 0;
        const AlertRule& x = r[row];
        switch (col) {
            case C_SIG:      return x.sig;
            case C_COND:     return x.cond;
            case C_THR:      return x.thr;
            case C_COOLDOWN: return x.cooldownS;
            case C_ON:       return x.on ? 1 : 0;
            default:         return 0;
        }
    }

    bool AlertRules::set(uint8_t row, uint8_t col, int32_t v)
    {
        if (row >= ROWS) return false;
        AlertRule& x = r[row];
        switch (col) {
            case C_SIG:
                if (v < 0 || v > SIG_COUNT) return false;
                x.sig = (uint8_t)v;
                return true;
            case C_COND:
                if (v < 0 || v >= AC_COUNT) return false;
                x.cond = (uint8_t)v;
                return true;
            case C_THR:
                if (x.sig < SIG_COUNT && (v < meta(x.sig).min || v > meta(x.sig).max)) return false;
                x.thr = v;
                return true;
            case C_COOLDOWN:
                if (v < 0 || (uint32_t)v > COOLDOWN_MAX_S || v > UINT16_MAX) return false;
                x.cooldownS = (uint16_t)v;
                return true;
            case C_ON:
                if (v != 0 && v != 1) return false;
                x.on = v == 1;
                return true;
            default:
                return false;
        }
    }

    size_t AlertRules::toBlob(uint8_t* out) const
    {
        uint8_t* p = out;
        *p++ = 1;
        for (const AlertRule& x : r) {
            *p++ = x.sig;
            *p++ = x.cond;
            const uint32_t t = (uint32_t)x.thr;
            *p++ = (uint8_t)t; *p++ = (uint8_t)(t >> 8); *p++ = (uint8_t)(t >> 16); *p++ = (uint8_t)(t >> 24);
            *p++ = (uint8_t)x.cooldownS; *p++ = (uint8_t)(x.cooldownS >> 8);
            *p++ = x.on ? 1 : 0;
        }
        return (size_t)(p - out);
    }

    bool AlertRules::fromBlob(const uint8_t* in, size_t n)
    {
        if (!in || n != BLOB_BYTES || in[0] != 1) return false;
        AlertRule t[ROWS];
        const uint8_t* p = in + 1;
        for (uint8_t i = 0; i < ROWS; ++i, p += ROW_BYTES) {
            AlertRule& x = t[i];
            x.sig = p[0];
            x.cond = p[1];
            x.thr = (int32_t)((uint32_t)p[2] | ((uint32_t)p[3] << 8) | ((uint32_t)p[4] << 16) | ((uint32_t)p[5] << 24));
            x.cooldownS = (uint16_t)(p[6] | (p[7] << 8));
            x.on = p[8] != 0;
            // A signal index past this build's list (a newer image's blob)
            // empties that row rather than refusing the table.
            if (x.sig > SIG_COUNT) x.sig = SIG_COUNT;
            if (x.cond >= AC_COUNT || p[8] > 1) return false;
        }
        for (uint8_t i = 0; i < ROWS; ++i) r[i] = t[i];
        return true;
    }

    // ---- the evaluator ------------------------------------------------------------

    bool AlertEval::same(const AlertRule& a, const AlertRule& b)
    {
        return a.sig == b.sig && a.cond == b.cond && a.thr == b.thr && a.cooldownS == b.cooldownS && a.on == b.on;
    }

    bool AlertEval::holds(const AlertRule& r, int32_t v, const Fire& f)
    {
        switch (r.cond) {
            case AC_ABOVE: return v > r.thr;
            case AC_BELOW: return v < r.thr;
            default:       return v == f.value;    // the edge's new value is still there
        }
    }

    void AlertEval::tick(const AlertRules& rules, const State& s, uint32_t now)
    {
        for (uint8_t i = 0; i < AlertRules::ROWS; ++i) {
            const AlertRule& r = rules.r[i];
            Row& st = _row[i];
            // An edited rule starts over: no memory of the old one's edges.
            if (!same(r, st.rule)) { st = Row{}; st.rule = r; }
            if (!r.on || r.sig >= SIG_COUNT || !s.has(r.sig)) continue;

            const Meta& m = meta(r.sig);
            const int32_t v = s.get(r.sig);
            int32_t band = (m.max - m.min) / 50;
            if (band < 1) band = 1;

            bool hit = false;
            switch (r.cond) {
                case AC_ABOVE:
                    hit = v > r.thr;
                    if (!st.armed && v <= r.thr - band) st.armed = true;
                    break;
                case AC_BELOW:
                    hit = v < r.thr;
                    if (!st.armed && v >= r.thr + band) st.armed = true;
                    break;
                case AC_RISES:   hit = st.seen ? v > st.last : (m.boolean && v != 0); break;
                case AC_FALLS:   hit = st.seen && v < st.last; break;
                case AC_CHANGES: hit = st.seen && v != st.last; break;
            }
            st.last = v;
            st.seen = true;

            const bool cooled = !st.everFired || (uint32_t)(now - st.lastFireMs) >= (uint32_t)r.cooldownS * 1000u;
            if (!hit || !st.armed || !cooled) continue;
            if (_has) ++_overwritten;
            _pend.row = i;
            _pend.sig = r.sig;
            _pend.value = v;
            _has = true;
            ++_fired;
            st.everFired = true;
            st.lastFireMs = now;
            if (r.cond == AC_ABOVE || r.cond == AC_BELOW) st.armed = false;
        }
    }

    bool AlertEval::take(const AlertRules& rules, const State& s, bool glassFree, Fire& out)
    {
        if (!_has) return false;
        const AlertRule& r = rules.r[_pend.row];
        if (r.sig != _pend.sig || !r.on || !s.has(_pend.sig) || !holds(r, s.get(_pend.sig), _pend)) {
            _has = false;
            ++_dropped;
            return false;
        }
        if (!glassFree) return false;
        out = _pend;
        _has = false;
        return true;
    }

    int AlertEval::text(char* b, size_t cap, uint8_t sig, int32_t v, bool verified)
    {
        const Meta& m = meta(sig);
        char num[24];
        if (m.decimals) {
            int32_t div = 1;
            for (uint8_t i = 0; i < m.decimals; ++i) div *= 10;
            const int32_t a = v < 0 ? -v : v;
            snprintf(num, sizeof(num), "%s%ld.%0*ld", v < 0 ? "-" : "", (long)(a / div), (int)m.decimals, (long)(a % div));
        } else {
            snprintf(num, sizeof(num), "%ld", (long)v);
        }
        // "0-3" and "raw" describe the range, not a unit: left off.
        const bool unit = ((m.unit[0] | 0x20) >= 'a' && (m.unit[0] | 0x20) <= 'z') && strcmp(m.unit, "raw") != 0;
        const int n = snprintf(b, cap, "%s: %s%s%s%s", m.name, num, unit ? " " : "", unit ? m.unit : "", verified ? "" : " ?");
        return n < 0 ? 0 : (size_t)n < cap ? n : (int)cap - 1;
    }
}
