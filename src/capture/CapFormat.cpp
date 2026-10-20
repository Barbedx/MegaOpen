#include "CapFormat.h"
#include <string.h>

namespace CapFormat
{
    namespace
    {
        void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
        void put32(uint8_t* p, uint32_t v)
        {
            p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
        }
        uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
        uint32_t get32(const uint8_t* p)
        {
            return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        }
    }

    void encodeHeader(const Header& h, uint8_t out[HEADER_SIZE])
    {
        memset(out, 0, HEADER_SIZE);
        memcpy(out, MAGIC, 6);
        put16(out + 6, (uint16_t)HEADER_SIZE);
        put32(out + 8, h.session);
        put16(out + 12, h.part);
        out[14] = h.mode;
        out[15] = (uint8_t)REC_SIZE;
        put32(out + 16, h.bitrate[0]);
        put32(out + 20, h.bitrate[1]);
        put32(out + 24, h.boot);
        put32(out + 28, h.wallSec);
        put32(out + 32, h.wallAtMs);
        put16(out + 36, h.keyframeMs);
        put16(out + 38, h.statsMs);
        // Always NUL-ended: 23 characters at most.
        size_t n = strnlen(h.build, sizeof(h.build));
        if (n > 23) n = 23;
        memcpy(out + 40, h.build, n);
    }

    bool decodeHeader(const uint8_t* in, size_t n, Header& h)
    {
        if (n < HEADER_SIZE || memcmp(in, MAGIC, 6) != 0) return false;
        if (get16(in + 6) < HEADER_SIZE || in[15] != REC_SIZE) return false;
        h.session    = get32(in + 8);
        h.part       = get16(in + 12);
        h.mode       = in[14];
        h.bitrate[0] = get32(in + 16);
        h.bitrate[1] = get32(in + 20);
        h.boot       = get32(in + 24);
        h.wallSec    = get32(in + 28);
        h.wallAtMs   = get32(in + 32);
        h.keyframeMs = get16(in + 36);
        h.statsMs    = get16(in + 38);
        memcpy(h.build, in + 40, 24);
        h.build[23] = 0;
        return true;
    }

    void encodeRec(const Rec& r, uint8_t out[REC_SIZE])
    {
        put32(out, r.t);
        put32(out + 4, r.w);
        out[8] = r.len;
        out[9] = r.kind;
        memcpy(out + 10, r.data, 8);
    }

    void decodeRec(const uint8_t in[REC_SIZE], Rec& r)
    {
        r.t    = get32(in);
        r.w    = get32(in + 4);
        r.len  = in[8];
        r.kind = in[9];
        memcpy(r.data, in + 10, 8);
    }

    // ---- markers -------------------------------------------------------------

    static size_t markLen(const char* text)
    {
        if (!text) return 0;
        const size_t n = strlen(text);
        return n > MARK_TEXT_MAX ? MARK_TEXT_MAX : n;
    }

    size_t markerPieces(const char* text)
    {
        const size_t n = markLen(text);
        return n == 0 ? 1 : (n + 7) / 8;
    }

    size_t markerRecs(uint16_t markerNo, const char* text, uint32_t t, Rec* out, size_t max)
    {
        const size_t n = markLen(text);
        const size_t pieces = markerPieces(text);
        if (pieces > max) return 0;
        for (size_t i = 0; i < pieces; ++i) {
            Rec& r = out[i];
            r = Rec();
            r.t = t;
            r.kind = MARKER;
            r.w = (uint32_t)markerNo | ((uint32_t)n << 16) | ((uint32_t)i << 24);
            const size_t off = i * 8;
            r.len = (uint8_t)(n - off < 8 ? n - off : 8);
            if (n) memcpy(r.data, text + off, r.len);
        }
        return pieces;
    }

    int markerText(const Rec* recs, size_t n, char* out, size_t cap)
    {
        if (!n || !cap || recs[0].kind != MARKER) return -1;
        const uint16_t no  = (uint16_t)(recs[0].w & 0xFFFF);
        const size_t total = (recs[0].w >> 16) & 0xFF;
        const size_t pieces = total == 0 ? 1 : (total + 7) / 8;
        if (n < pieces) return -1;
        size_t got = 0;
        for (size_t i = 0; i < pieces; ++i) {
            const Rec& r = recs[i];
            if (r.kind != MARKER || (r.w & 0xFFFF) != no || ((r.w >> 24) & 0xFF) != i) return -1;
            for (uint8_t k = 0; k < r.len && k < 8; ++k)
                if (got + 1 < cap) out[got++] = (char)r.data[k];
        }
        out[got] = 0;
        return (int)got;
    }

    // ---- change-only filter ----------------------------------------------------

    void ChangeFilter::begin(Entry* table, uint16_t cap, uint16_t keyframeMs)
    {
        _t = table;
        _cap = cap;
        _keyMs = keyframeMs;
        reset();
    }

    void ChangeFilter::reset()
    {
        if (_t) memset(_t, 0, sizeof(Entry) * _cap);
        _ids = 0;
    }

    void ChangeFilter::rekey()
    {
        if (!_t) return;
        for (uint16_t i = 0; i < _cap; ++i)
            if (_t[i].used) _t[i].used = 1;
    }

    ChangeFilter::Entry* ChangeFilter::find(uint32_t w, bool insert)
    {
        if (!_t || !_cap) return nullptr;
        const uint16_t mask = (uint16_t)(_cap - 1);
        uint16_t i = (uint16_t)((w * 2654435761u) >> 16) & mask;
        for (uint16_t probe = 0; probe < _cap; ++probe, i = (uint16_t)((i + 1) & mask)) {
            Entry& e = _t[i];
            if (e.used && e.w == w) return &e;
            if (!e.used) {
                // Keep one slot in eight free: a full open table probes forever.
                if (!insert || _ids >= _cap - _cap / 8) return nullptr;
                e.used = 1;
                e.w = w;
                ++_ids;
                return &e;
            }
        }
        return nullptr;
    }

    uint8_t ChangeFilter::onFrame(const Rec& r)
    {
        Entry* e = find(r.w, true);
        if (!e) { ++_overflow; return FRAME; }

        const bool fresh = e->used == 1;   // in the table, never written
        const uint8_t len = r.len > 8 ? 8 : r.len;
        const bool changed = e->len != len || memcmp(e->data, r.data, len) != 0;
        ++e->count;
        if (changed) ++e->changes;

        uint8_t kind = SKIP;
        // First seen is a change, whatever its bytes.
        if (fresh || changed)                                kind = FRAME;
        else if ((uint32_t)(r.t - e->lastWriteMs) >= _keyMs) kind = KEYFRAME;

        if (kind != SKIP) {
            e->used = 2;
            e->lastWriteMs = r.t;
            e->len = len;
            memset(e->data, 0, 8);
            memcpy(e->data, r.data, len);
        }
        return kind;
    }

    size_t ChangeFilter::emitStats(uint32_t nowMs, Out out, void* ctx)
    {
        const uint32_t span = nowMs - _statsAt;
        const uint16_t ms = span > 0xFFFF ? 0xFFFF : (uint16_t)span;
        _statsAt = nowMs;
        size_t n = 0;
        if (!_t) return 0;
        for (uint16_t i = 0; i < _cap; ++i) {
            Entry& e = _t[i];
            if (!e.used || !e.count) continue;
            Rec r;
            r.t = nowMs;
            r.w = e.w;
            r.kind = STATS;
            r.len = 8;
            r.data[0] = (uint8_t)e.count;         r.data[1] = (uint8_t)(e.count >> 8);
            r.data[2] = (uint8_t)(e.count >> 16); r.data[3] = (uint8_t)(e.count >> 24);
            r.data[4] = (uint8_t)ms;              r.data[5] = (uint8_t)(ms >> 8);
            r.data[6] = (uint8_t)e.changes;       r.data[7] = (uint8_t)(e.changes >> 8);
            if (out) out(r, ctx);
            e.count = 0;
            e.changes = 0;
            ++n;
        }
        return n;
    }
}
