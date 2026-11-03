#include "FieldMap.h"

namespace Canbox
{
    const FieldRule* FieldMap::find(uint8_t field) const
    {
        for (uint8_t i = 0; i < _n; ++i)
            if (_r[i].field == field) return &_r[i];
        return nullptr;
    }

    bool FieldMap::set(const FieldRule& r)
    {
        if (r.field == 0xFF || r.kind < FK_SIGNAL || r.kind > FK_OFF) return false;
        if (r.kind == FK_SIGNAL && (r.src == 0xFF || r.div == 0)) return false;
        for (uint8_t i = 0; i < _n; ++i)
            if (_r[i].field == r.field) { _r[i] = r; return true; }
        if (_n >= MAX) return false;
        _r[_n++] = r;
        return true;
    }

    void FieldMap::clear(uint8_t field)
    {
        for (uint8_t i = 0; i < _n; ++i) {
            if (_r[i].field != field) continue;
            for (uint8_t j = i + 1; j < _n; ++j) _r[j - 1] = _r[j];
            --_n;
            return;
        }
    }

    int32_t FieldMap::scale(const FieldRule& r, int32_t v)
    {
        int64_t x = (int64_t)v * r.mul;
        if (r.div) {   // to the nearest, halves away from zero (12.65 V x10 -> 127)
            const int64_t d = r.div < 0 ? -(int64_t)r.div : r.div;
            x = (x >= 0 ? x + d / 2 : x - d / 2) / d;
            if (r.div < 0) x = -x;
        }
        x += r.add;
        if (x > INT32_MAX) x = INT32_MAX;
        if (x < INT32_MIN) x = INT32_MIN;
        return (int32_t)x;
    }
}
