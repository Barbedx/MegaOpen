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

    // The 64 bytes v1 and v2 share; magic, length and record size differ.
    static void encodeCommon(const Header& h, uint8_t* out, const char* magic, uint16_t len, uint8_t recSize)
    {
        memset(out, 0, len);
        memcpy(out, magic, 6);
        put16(out + 6, len);
        put32(out + 8, h.session);
        put16(out + 12, h.part);
        out[14] = h.mode;
        out[15] = recSize;
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

    void encodeHeader(const Header& h, uint8_t out[HEADER_SIZE])
    {
        encodeCommon(h, out, MAGIC, (uint16_t)HEADER_SIZE, (uint8_t)REC_SIZE);
    }

    void encodeHeader2(const Header& h, uint8_t out[HEADER2_SIZE])
    {
        encodeCommon(h, out, MAGIC2, (uint16_t)HEADER2_SIZE, 0);
        put32(out + 64, h.t0);
        put32(out + 68, h.segLen);
        put32(out + 72, h.seq);
        put32(out + 76, h.records);
    }

    bool decodeHeader(const uint8_t* in, size_t n, Header& h)
    {
        if (n < HEADER_SIZE) return false;
        const bool v2 = memcmp(in, MAGIC2, 6) == 0;
        if (!v2 && memcmp(in, MAGIC, 6) != 0) return false;
        const uint16_t hl = get16(in + 6);
        if (v2) {
            if (n < HEADER2_SIZE || hl < HEADER2_SIZE || in[15] != 0) return false;
        } else if (hl < HEADER_SIZE || in[15] != REC_SIZE) {
            return false;
        }
        h.version    = v2 ? 2 : 1;
        h.headerLen  = hl;
        h.t0         = v2 ? get32(in + 64) : 0;
        h.segLen     = v2 ? get32(in + 68) : 0;
        h.seq        = v2 ? get32(in + 72) : 0;
        h.records    = v2 ? get32(in + 76) : 0;
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

    // ---- v2 segments -------------------------------------------------------------

    namespace
    {
        size_t putVar(uint8_t* p, uint32_t v)
        {
            size_t n = 0;
            while (v >= 0x80) { p[n++] = (uint8_t)(v | 0x80); v >>= 7; }
            p[n++] = (uint8_t)v;
            return n;
        }
        // False when the varint runs past end (or past 5 bytes).
        bool getVar(const uint8_t* in, size_t end, size_t& q, uint32_t& v)
        {
            v = 0;
            for (int s = 0; s < 35; s += 7) {
                if (q >= end) return false;
                const uint8_t b = in[q++];
                v |= (uint32_t)(b & 0x7F) << s;
                if (!(b & 0x80)) return true;
            }
            return false;
        }
        uint32_t zig(int32_t v)    { return ((uint32_t)v << 1) ^ (uint32_t)(v >> 31); }
        int32_t  unzig(uint32_t v) { return (int32_t)(v >> 1) ^ -(int32_t)(v & 1); }
        uint16_t hashOf(uint32_t w) { return (uint16_t)(((w * 2654435761u) >> 16) & 511); }
        uint8_t  popcount8(uint8_t m) { uint8_t c = 0; while (m) { c += m & 1; m >>= 1; } return c; }
    }

    void SegEncoder::reset()
    {
        _nIds = 0;
        if (_m) memset(_m->hash, 0xFF, sizeof(_m->hash));
    }

    int SegEncoder::find(uint32_t w) const
    {
        if (!_m) return -1;
        uint16_t i = hashOf(w);
        for (uint16_t probe = 0; probe < 512; ++probe, i = (uint16_t)((i + 1) & 511)) {
            const uint8_t k = _m->hash[i];
            if (k == 0xFF) return -1;
            if (_m->ids[k].w == w) return k;
        }
        return -1;
    }

    int SegEncoder::insert(uint32_t w)
    {
        if (!_m || _nIds >= SEG_IDS) return -1;
        uint16_t i = hashOf(w);
        while (_m->hash[i] != 0xFF) i = (uint16_t)((i + 1) & 511);   // half full at most: ends
        Id& e = _m->ids[_nIds];
        e.w = w; e.len = 0; e.seen = 0;
        memset(e.data, 0, 8);
        _m->hash[i] = (uint8_t)_nIds;
        return _nIds++;
    }

    void SegEncoder::patch()
    {
        put32(_buf + 68, (uint32_t)_n);
        put32(_buf + 76, _recs);
    }

    bool SegEncoder::open(uint8_t* buf, size_t cap, const Header& h)
    {
        if (!buf || !_m || cap < SEG_HEAD_MAX + REC2_MAX) return false;
        _buf = buf;
        _cap = cap;
        _recs = 0;
        _t0 = _last = h.t0;
        _statsKnown = false;
        Header hh = h;
        hh.segLen = 0;
        hh.records = 0;
        encodeHeader2(hh, buf);
        _n = HEADER2_SIZE;
        buf[_n++] = (uint8_t)_nIds;
        for (uint16_t i = 0; i < _nIds; ++i) {
            put32(buf + _n, _m->ids[i].w);
            _n += 4;
            _m->ids[i].seen = 0;   // every id's first record here carries its data
        }
        patch();
        return true;
    }

    bool SegEncoder::add(const Rec& r)
    {
        if (!_buf || _n + REC2_MAX > _cap) return false;
        uint8_t* p = _buf + _n;
        size_t k = 1;
        uint8_t b0 = (uint8_t)(r.kind & 7);
        const uint32_t z = zig((int32_t)(r.t - _last));
        if (z < DT_ESC) b0 |= (uint8_t)(z << 4);
        else { b0 |= (uint8_t)(DT_ESC << 4); k += putVar(p + k, z); }

        switch (r.kind) {
            case FRAME: case KEYFRAME: case STATS: {
                int idx = find(r.w);
                if (idx < 0) {
                    idx = insert(r.w);   // -1: past the table, an escape every time
                    p[k++] = IDX_ESC;
                    put32(p + k, r.w);
                    k += 4;
                } else {
                    p[k++] = (uint8_t)idx;
                }
                if (r.kind == STATS) {
                    k += putVar(p + k, get32(r.data));
                    k += putVar(p + k, get16(r.data + 6));
                    const uint16_t ms = get16(r.data + 4);
                    if (!_statsKnown || ms != _statsMs) {
                        b0 |= F_FULL;
                        k += putVar(p + k, ms);
                        _statsMs = ms;
                        _statsKnown = true;
                    }
                    break;
                }
                const uint8_t len = r.len > 8 ? 8 : r.len;
                Id* e = idx >= 0 ? &_m->ids[idx] : nullptr;
                if (!e || !e->seen || e->len != len) {
                    b0 |= F_FULL;
                    p[k++] = len;
                    memcpy(p + k, r.data, len);
                    k += len;
                } else {
                    const size_t at = k++;
                    uint8_t mask = 0;
                    for (uint8_t i = 0; i < len; ++i)
                        if (r.data[i] != e->data[i]) { mask |= (uint8_t)(1u << i); p[k++] = r.data[i]; }
                    p[at] = mask;
                }
                if (e) {
                    e->seen = 1;
                    e->len = len;
                    memset(e->data, 0, 8);
                    memcpy(e->data, r.data, len);
                }
                break;
            }
            case MARKER: {
                const uint8_t len = r.len > 8 ? 8 : r.len;
                k += putVar(p + k, r.w & 0xFFFF);
                p[k++] = (uint8_t)(r.w >> 16);
                p[k++] = (uint8_t)(r.w >> 24);
                p[k++] = len;
                memcpy(p + k, r.data, len);
                k += len;
                break;
            }
            case GAP:
                if (r.w == 1) b0 |= F_FULL;
                k += putVar(p + k, get32(r.data));
                break;
            case BITRATE:
                p[k++] = busOf(r.w);
                k += putVar(p + k, get32(r.data));
                break;
            case MODE:
                p[k++] = r.data[0];
                break;
            default:
                return true;   // no reader knows it: not written
        }
        p[0] = b0;
        _n += k;
        _last = r.t;
        ++_recs;
        patch();
        return true;
    }

    size_t SegDecoder::segment(const uint8_t* in, size_t n, Header& h, Out out, void* ctx, uint32_t* recs)
    {
        if (recs) *recs = 0;
        if (!in || !decodeHeader(in, n, h) || h.version != 2) return 0;
        const size_t end = h.segLen && h.segLen <= n ? h.segLen : n;
        size_t o = h.headerLen;
        if (o >= end) return end;
        const uint8_t cnt = in[o];
        if (o + 1 + 4u * cnt > end) return o;
        ++o;
        _n = 0;
        for (uint16_t i = 0; i < cnt; ++i, o += 4) {
            _ids[_n].w = get32(in + o);
            _ids[_n].len = 0;
            memset(_ids[_n].data, 0, 8);
            ++_n;
        }
        uint32_t t = h.t0, statsMs = 0, got = 0;
        while (o < end) {
            size_t q = o;
            const uint8_t b0 = in[q++];
            const uint8_t kind = b0 & 7;
            const bool full = (b0 & F_FULL) != 0;
            uint32_t z = b0 >> 4;
            if (z == DT_ESC && !getVar(in, end, q, z)) break;
            Rec r;
            r.t = t + (uint32_t)unzig(z);
            r.kind = kind;
            bool ok = true;
            switch (kind) {
                case FRAME: case KEYFRAME: case STATS: {
                    if (q >= end) { ok = false; break; }
                    int idx = -1;
                    uint32_t w = 0;
                    bool esc = false;
                    if (in[q] == IDX_ESC) {
                        if (q + 5 > end) { ok = false; break; }
                        w = get32(in + q + 1);
                        q += 5;
                        esc = true;
                    } else {
                        idx = in[q++];
                        if (idx >= _n) { ok = false; break; }
                        w = _ids[idx].w;
                    }
                    uint8_t tmp[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                    uint8_t len = 0, mask = 0;
                    uint32_t cnt2 = 0, ch = 0, ms = statsMs;
                    if (kind == STATS) {
                        if (!getVar(in, end, q, cnt2) || !getVar(in, end, q, ch)) { ok = false; break; }
                        if (full && !getVar(in, end, q, ms)) { ok = false; break; }
                    } else if (full) {
                        if (q >= end) { ok = false; break; }
                        len = in[q++];
                        if (len > 8 || q + len > end) { ok = false; break; }
                        memcpy(tmp, in + q, len);
                        q += len;
                    } else {
                        if (q >= end) { ok = false; break; }
                        mask = in[q++];
                        const uint8_t c = popcount8(mask);
                        if (q + c > end) { ok = false; break; }
                        memcpy(tmp, in + q, c);
                        q += c;
                    }
                    // Whole: now the table moves.
                    if (esc) {
                        for (uint16_t i = 0; i < _n; ++i) if (_ids[i].w == w) { idx = i; break; }
                        if (idx < 0 && _n < SEG_IDS) {
                            idx = _n++;
                            _ids[idx].w = w;
                            _ids[idx].len = 0;
                            memset(_ids[idx].data, 0, 8);
                        }
                    }
                    r.w = w;
                    if (kind == STATS) {
                        statsMs = ms;
                        r.len = 8;
                        put32(r.data, cnt2);
                        put16(r.data + 4, (uint16_t)ms);
                        put16(r.data + 6, (uint16_t)ch);
                        break;
                    }
                    if (full) {
                        r.len = len;
                        memcpy(r.data, tmp, 8);
                    } else {
                        const Id* e = idx >= 0 ? &_ids[idx] : nullptr;
                        r.len = e ? e->len : 0;
                        if (e) memcpy(r.data, e->data, 8);
                        uint8_t c = 0;
                        for (uint8_t i = 0; i < 8; ++i)
                            if (mask & (1u << i)) r.data[i] = tmp[c++];
                    }
                    if (idx >= 0) {
                        _ids[idx].len = r.len;
                        memcpy(_ids[idx].data, r.data, 8);
                    }
                    break;
                }
                case MARKER: {
                    uint32_t no = 0;
                    if (!getVar(in, end, q, no) || q + 3 > end) { ok = false; break; }
                    const uint8_t total = in[q], piece = in[q + 1], len = in[q + 2];
                    q += 3;
                    if (len > 8 || q + len > end) { ok = false; break; }
                    r.w = (no & 0xFFFF) | ((uint32_t)total << 16) | ((uint32_t)piece << 24);
                    r.len = len;
                    memcpy(r.data, in + q, len);
                    q += len;
                    break;
                }
                case GAP: case BITRATE: {
                    uint32_t v = 0;
                    uint8_t bus = 0;
                    if (kind == BITRATE) {
                        if (q >= end) { ok = false; break; }
                        bus = in[q++];
                    }
                    if (!getVar(in, end, q, v)) { ok = false; break; }
                    const uint32_t w = kind == GAP ? (full ? 1u : 0u) : (uint32_t)(bus & 3) << BUS_SHIFT;
                    r = u32Rec(kind, r.t, w, v);
                    break;
                }
                case MODE:
                    if (q >= end) { ok = false; break; }
                    r = u32Rec(MODE, r.t, 0, in[q++]);
                    r.len = 1;
                    break;
                default:
                    ok = false;   // not a kind of this version: the rest is unreadable
                    break;
            }
            if (!ok) break;
            t = r.t;
            o = q;
            ++got;
            if (out) out(r, ctx);
        }
        if (recs) *recs = got;
        return o;
    }

    uint32_t SegDecoder::stream(const uint8_t* in, size_t n, Out out, void* ctx, uint32_t* segs)
    {
        uint32_t total = 0, nSegs = 0;
        size_t o = 0;
        while (o < n) {
            Header h;
            uint32_t got = 0;
            const size_t used = segment(in + o, n - o, h, out, ctx, &got);
            if (!used) break;
            total += got;
            ++nSegs;
            // Cut short: nothing after it can be trusted as a segment start.
            if (!h.segLen || h.segLen < h.headerLen || h.segLen > n - o || used < h.segLen) break;
            o += h.segLen;
        }
        if (segs) *segs = nSegs;
        return total;
    }
}
