#include "DecodeTable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace Vehicle
{
    namespace
    {
        // CanDecode's sort key: 11-bit and 29-bit ids apart.
        inline uint32_t key(const DecodeRow& r)
        {
            return (r.flags & DF_EXT) ? (r.canId | 0x80000000u) : r.canId;
        }

        // Insertion, so rows of one id keep their order (stable).
        void sortRows(DecodeRow* r, uint8_t n)
        {
            for (uint8_t i = 1; i < n; ++i) {
                const DecodeRow x = r[i];
                uint8_t j = i;
                while (j && key(r[j - 1]) > key(x)) { r[j] = r[j - 1]; --j; }
                r[j] = x;
            }
        }

        uint8_t* put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); return p + 2; }
        uint8_t* put32(uint8_t* p, uint32_t v) { p = put16(p, (uint16_t)v); return put16(p, (uint16_t)(v >> 16)); }
        uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
        uint32_t get32(const uint8_t* p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }

        // "0x354", "354" or a number.
        bool hexOf(JsonVariantConst v, uint32_t& out)
        {
            if (v.is<uint32_t>()) { out = v.as<uint32_t>(); return true; }
            const char* s = v.as<const char*>();
            if (!s || !*s) return false;
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
            if (!*s || strlen(s) > 8) return false;
            char* end = nullptr;
            const unsigned long x = strtoul(s, &end, 16);
            if (!end || *end) return false;
            out = (uint32_t)x;
            return true;
        }

        // An integer member in lo..hi, `dflt` when absent.
        bool intOf(JsonObjectConst o, const char* k, int32_t dflt, int32_t lo, int32_t hi, int32_t& out)
        {
            JsonVariantConst v = o[k];
            if (v.isNull()) { out = dflt; return true; }
            if (!v.is<int32_t>()) return false;
            out = v.as<int32_t>();
            return out >= lo && out <= hi;
        }
    }

    // ---- the table -------------------------------------------------------------

    void DecodeTable::factory()
    {
        uint8_t n = 0;
        const DecodeRow* d = CanDecode::defaults(n);
        nRows = n < MAX_ROWS ? n : MAX_ROWS;
        for (uint8_t i = 0; i < nRows; ++i) row[i] = d[i];
        nK = 0;
    }

    int DecodeTable::rowOf(uint8_t sig) const
    {
        for (uint8_t i = 0; i < nRows; ++i) if (row[i].sig == sig) return i;
        return -1;
    }

    int DecodeTable::constOf(uint8_t sig) const
    {
        for (uint8_t i = 0; i < nK; ++i) if (k[i].sig == sig) return i;
        return -1;
    }

    bool DecodeTable::measured(uint8_t sig) const
    {
        bool any = false;
        for (uint8_t i = 0; i < nRows; ++i) {
            if (row[i].sig != sig) continue;
            if (row[i].flags & DF_HYPOTHESIS) return false;
            any = true;
        }
        return any;
    }

    bool DecodeTable::setMeasured(uint8_t sig, bool on)
    {
        uint8_t nd = 0;
        const DecodeRow* d = CanDecode::defaults(nd);
        bool any = false;
        for (uint8_t i = 0; i < nRows; ++i) {
            if (row[i].sig != sig) continue;
            bool hyp = !on;
            for (uint8_t j = 0; !on && j < nd; ++j)
                if (d[j].sig == sig && sameField(d[j], row[i])) { hyp = (d[j].flags & DF_HYPOTHESIS) != 0; break; }
            row[i].flags = hyp ? (uint8_t)(row[i].flags | DF_HYPOTHESIS) : (uint8_t)(row[i].flags & ~DF_HYPOTHESIS);
            any = true;
        }
        return any;
    }

    const char* DecodeTable::apply(const DecodeChange& c, DecodeTable& s)
    {
        s = *this;
        if (c.reset) s.factory();
        // Every edited signal unbound first, so the room is the final
        // table's (one edit freeing a constant another one takes).
        for (uint8_t i = 0; i < c.n; ++i) s.unbind(c.ed[i].sig);
        for (uint8_t i = 0; i < c.n; ++i) {
            const DecodeEdit& e = c.ed[i];
            if (e.src == SRC_CAN) {
                // A latch or a key flag is the row's own behaviour, which the
                // page does not edit: kept from the old row (the table before
                // this request, or the factory's after a reset).
                const DecodeRow* old = nullptr;
                if (c.reset) {
                    uint8_t nd = 0;
                    const DecodeRow* d = CanDecode::defaults(nd);
                    for (uint8_t j = 0; !old && j < nd; ++j) if (d[j].sig == e.sig) old = &d[j];
                } else if (rowOf(e.sig) >= 0) {
                    old = &row[rowOf(e.sig)];
                }
                DecodeRow r = e.row;
                if (old) {
                    r.flags |= old->flags & (DF_LATCH | DF_KEY);
                    if (old->flags & DF_LATCH) r.holdMs = old->holdMs;
                    // Re-saving the same field is not a measurement: a
                    // hypothesis stays one, and a 2-bit state keeps its
                    // reading. A changed field is the user's own, so it
                    // counts as measured and reads plain.
                    if (sameField(r, *old))
                        r.flags |= old->flags & (DF_HYPOTHESIS | DF_STATE2 | DF_STATE2_INV);
                }
                if (!s.bindCan(e.sig, r)) return WHY_ROWS;
            } else if (e.src == SRC_CONST) {
                if (!s.bindConst(e.sig, e.k)) return WHY_CONSTS;
            }
        }
        if (c.hasConf && !s.setMeasured(c.confSig, c.confOn)) return WHY_NO_CAN;
        *this = s;
        return nullptr;
    }

    bool DecodeTable::factoryMeasured(uint8_t sig)
    {
        uint8_t n = 0;
        const DecodeRow* d = CanDecode::defaults(n);
        bool any = false;
        for (uint8_t i = 0; i < n; ++i) {
            if (d[i].sig != sig) continue;
            if (d[i].flags & DF_HYPOTHESIS) return false;
            any = true;
        }
        return any;
    }

    bool DecodeTable::sameField(const DecodeRow& a, const DecodeRow& b)
    {
        const uint8_t how = DF_LE | DF_SIGNED | DF_EXT;
        return a.canId == b.canId && a.startByte == b.startByte && a.numBytes == b.numBytes &&
               a.shift == b.shift && a.mask == b.mask && a.mul == b.mul &&
               (a.div ? a.div : 1) == (b.div ? b.div : 1) && a.add == b.add &&
               (a.flags & how) == (b.flags & how);
    }

    void DecodeTable::unbind(uint8_t sig)
    {
        uint8_t w = 0;
        for (uint8_t i = 0; i < nRows; ++i) if (row[i].sig != sig) row[w++] = row[i];
        nRows = w;
        w = 0;
        for (uint8_t i = 0; i < nK; ++i) if (k[i].sig != sig) k[w++] = k[i];
        nK = w;
    }

    bool DecodeTable::bindCan(uint8_t sig, const DecodeRow& r)
    {
        if (sig >= SIG_COUNT) return false;
        uint8_t others = 0;
        for (uint8_t i = 0; i < nRows; ++i) if (row[i].sig != sig) ++others;
        if (others >= MAX_ROWS) return false;
        unbind(sig);
        row[nRows] = r;
        row[nRows].sig = sig;
        ++nRows;
        sortRows(row, nRows);
        return true;
    }

    bool DecodeTable::bindConst(uint8_t sig, int32_t value)
    {
        if (sig >= SIG_COUNT) return false;
        if (constOf(sig) < 0 && nK >= MAX_CONSTS) return false;
        unbind(sig);
        k[nK].sig = sig;
        k[nK].value = value;
        ++nK;
        return true;
    }

    size_t DecodeTable::pack(uint8_t* out) const
    {
        uint8_t* p = out;
        *p++ = 1;
        *p++ = nRows;
        *p++ = nK;
        for (uint8_t i = 0; i < nRows; ++i) {
            const DecodeRow& r = row[i];
            p = put32(p, r.canId);
            *p++ = r.startByte; *p++ = r.numBytes; *p++ = r.shift;
            p = put32(p, r.mask);
            p = put16(p, (uint16_t)r.mul);
            p = put16(p, (uint16_t)r.div);
            p = put32(p, (uint32_t)r.add);
            *p++ = r.sig; *p++ = r.flags;
            p = put16(p, r.holdMs);
        }
        for (uint8_t i = 0; i < nK; ++i) {
            *p++ = k[i].sig;
            p = put32(p, (uint32_t)k[i].value);
        }
        return (size_t)(p - out);
    }

    bool DecodeTable::unpack(const uint8_t* in, size_t n)
    {
        nRows = 0;
        nK = 0;
        if (!in || n < 3 || in[0] != 1 || in[1] > MAX_ROWS || in[2] > MAX_CONSTS) return false;
        if (n != 3 + (size_t)in[1] * ROW_BYTES + (size_t)in[2] * CONST_BYTES) return false;
        const uint8_t* p = in + 3;
        for (uint8_t i = 0; i < in[1]; ++i, p += ROW_BYTES) {
            DecodeRow& r = row[i];
            r.canId = get32(p);
            r.startByte = p[4]; r.numBytes = p[5]; r.shift = p[6];
            r.mask = get32(p + 7);
            r.mul = (int16_t)get16(p + 11);
            r.div = (int16_t)get16(p + 13);
            r.add = (int32_t)get32(p + 15);
            r.sig = p[19]; r.flags = p[20];
            r.holdMs = get16(p + 21);
            if (r.sig >= SIG_COUNT || r.numBytes < 1 || r.numBytes > 4 || r.startByte + r.numBytes > 8 ||
                r.shift > 31 || !r.mask || !r.mul || !r.div)
                return false;
        }
        const uint8_t rows = in[1];
        for (uint8_t i = 0; i < in[2]; ++i, p += CONST_BYTES) {
            const uint8_t sig = p[0];
            const int32_t v = (int32_t)get32(p + 1);
            if (sig >= SIG_COUNT || v < meta(sig).min || v > meta(sig).max) return false;
            for (uint8_t j = 0; j < rows; ++j) if (row[j].sig == sig) return false;
            for (uint8_t j = 0; j < i; ++j) if (k[j].sig == sig) return false;
            k[i].sig = sig;
            k[i].value = v;
        }
        sortRows(row, rows);
        nRows = rows;
        nK = in[2];
        return true;
    }

    // ---- the `can` object ---------------------------------------------------------

    namespace DecodeJson
    {
        int canJson(char* b, size_t cap, const DecodeRow& r)
        {
            const bool ext = (r.flags & DF_EXT) != 0;
            const int n = snprintf(b, cap,
                                   "{\"id\":\"0x%0*lX\",\"byte\":%u,\"len\":%u,\"shift\":%u,\"mask\":\"%lX\","
                                   "\"mul\":%d,\"div\":%d,\"add\":%ld,\"le\":%s,\"signed\":%s,\"hyp\":%s}",
                                   ext ? 8 : 3, (unsigned long)r.canId, (unsigned)r.startByte,
                                   (unsigned)r.numBytes, (unsigned)r.shift, (unsigned long)r.mask,
                                   (int)r.mul, (int)(r.div ? r.div : 1), (long)r.add,
                                   (r.flags & DF_LE) ? "true" : "false",
                                   (r.flags & DF_SIGNED) ? "true" : "false",
                                   (r.flags & DF_HYPOTHESIS) ? "true" : "false");
            return n < 0 ? 0 : (size_t)n < cap ? n : (int)cap - 1;
        }

        const char* canFrom(JsonObjectConst o, DecodeRow& r)
        {
            uint32_t id = 0;
            if (!hexOf(o["id"], id) || id > 0x1FFFFFFFu) return "id: a CAN id, like 0x354";
            int32_t b, len, shift, mul, div, add;
            if (!intOf(o, "byte", -1, 0, 7, b)) return "byte: 0..7";
            if (!intOf(o, "len", 1, 1, 4, len) || b + len > 8) return "len: 1..4 bytes, inside the 8";
            if (!intOf(o, "shift", 0, 0, 31, shift)) return "shift: 0..31";
            uint32_t mask = 0;
            if (!hexOf(o["mask"], mask) || !mask) return "mask: hex, not 0";
            if (!intOf(o, "mul", 1, -32768, 32767, mul) || !mul) return "mul: a whole number, not 0, within 16 bits";
            if (!intOf(o, "div", 1, -32768, 32767, div) || !div) return "div: a whole number, not 0, within 16 bits";
            if (!intOf(o, "add", 0, INT32_MIN, INT32_MAX, add)) return "add: a whole number";

            r.canId     = id;
            r.startByte = (uint8_t)b;
            r.numBytes  = (uint8_t)len;
            r.shift     = (uint8_t)shift;
            r.mask      = mask;
            r.mul       = (int16_t)mul;
            r.div       = (int16_t)div;
            r.add       = add;
            r.flags     = 0;
            if (o["le"] | false)     r.flags |= DF_LE;
            if (o["signed"] | false) r.flags |= DF_SIGNED;
            if (o["hyp"] | false)    r.flags |= DF_HYPOTHESIS;
            if (id > 0x7FF)          r.flags |= DF_EXT;
            return nullptr;
        }
    }
}
