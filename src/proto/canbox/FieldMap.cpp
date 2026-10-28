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

    size_t FieldMap::pack(uint8_t* out, size_t cap) const
    {
        const size_t need = 2 + (size_t)_n * RULE_BYTES;
        if (!out || cap < need) return 0;
        uint8_t* p = out;
        *p++ = BLOB_VER;
        *p++ = _n;
        for (uint8_t i = 0; i < _n; ++i) {
            const FieldRule& r = _r[i];
            *p++ = r.field; *p++ = r.kind; *p++ = r.src;
            *p++ = (uint8_t)r.mul; *p++ = (uint8_t)((uint16_t)r.mul >> 8);
            *p++ = (uint8_t)r.div; *p++ = (uint8_t)((uint16_t)r.div >> 8);
            const uint32_t a = (uint32_t)r.add;
            *p++ = (uint8_t)a; *p++ = (uint8_t)(a >> 8); *p++ = (uint8_t)(a >> 16); *p++ = (uint8_t)(a >> 24);
        }
        return need;
    }

    bool FieldMap::unpack(const uint8_t* in, size_t n)
    {
        if (!in || n < 2 || in[0] != BLOB_VER || in[1] > MAX || n != 2 + (size_t)in[1] * RULE_BYTES) return false;
        FieldMap m;
        const uint8_t* p = in + 2;
        for (uint8_t i = 0; i < in[1]; ++i, p += RULE_BYTES) {
            FieldRule r;
            r.field = p[0]; r.kind = p[1]; r.src = p[2];
            r.mul = (int16_t)(p[3] | (p[4] << 8));
            r.div = (int16_t)(p[5] | (p[6] << 8));
            r.add = (int32_t)((uint32_t)p[7] | ((uint32_t)p[8] << 8) | ((uint32_t)p[9] << 16) | ((uint32_t)p[10] << 24));
            if (!m.set(r)) return false;
        }
        *this = m;
        return true;
    }
}
