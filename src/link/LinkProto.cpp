#include "LinkProto.h"
#include <string.h>

namespace Link
{
    // CRC-8/ATM (poly 0x07), the one every reference table agrees on.
    uint8_t crc8(const uint8_t* p, size_t n)
    {
        uint8_t c = 0;
        for (size_t i = 0; i < n; ++i) {
            c ^= p[i];
            for (uint8_t k = 0; k < 8; ++k) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : (c << 1));
        }
        return c;
    }

    static uint8_t seal(uint8_t type, const uint8_t* payload, uint8_t len, uint8_t* out)
    {
        out[0] = SYNC;
        out[1] = type;
        out[2] = len;
        if (len) memcpy(out + 3, payload, len);
        out[3 + len] = crc8(out + 1, (size_t)len + 2);
        return (uint8_t)(4 + len);
    }

    static void put32(uint8_t* p, uint32_t v)
    {
        p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
    }
    static uint32_t get32(const uint8_t* p)
    {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    }

    uint8_t encodeCan(const Frame& f, uint8_t* out)
    {
        uint8_t p[13];
        const uint8_t dlc = f.len > 8 ? 8 : f.len;
        put32(p, (f.id & 0x1FFFFFFFu) | (f.extended ? 0x80000000u : 0));
        p[4] = dlc;
        for (uint8_t i = 0; i < dlc; ++i) p[5 + i] = f.data[i];
        return seal(T_CAN, p, (uint8_t)(5 + dlc), out);
    }

    uint8_t encodeStatus(const Status& s, uint8_t* out)
    {
        uint8_t p[19];
        put32(p, s.frames);
        put32(p + 4, s.forwarded);
        p[8] = (uint8_t)(s.fps >> 8); p[9] = (uint8_t)s.fps;
        p[10] = s.rxErr;
        p[11] = s.mode;
        put32(p + 12, s.busErr);
        p[16] = s.state;
        p[17] = (uint8_t)(s.sim >> 8); p[18] = (uint8_t)s.sim;
        return seal(T_STATUS, p, 19, out);
    }

    uint8_t encodeHello(uint8_t version, uint8_t* out)
    {
        return seal(T_HELLO, &version, 1, out);
    }

    uint8_t encodeFilter(FilterMode mode, const uint32_t* ids, uint8_t n, uint8_t* out)
    {
        if (n > MAX_FILTER_IDS) n = MAX_FILTER_IDS;
        uint8_t p[2 + 4 * MAX_FILTER_IDS];
        p[0] = (uint8_t)mode;
        p[1] = n;
        for (uint8_t i = 0; i < n; ++i) put32(p + 2 + 4 * i, ids[i]);
        return seal(T_FILTER, p, (uint8_t)(2 + 4 * n), out);
    }

    bool Record::asCan(Frame& f) const
    {
        if (type != T_CAN || len < 5) return false;
        const uint32_t raw = get32(payload);
        const uint8_t dlc = payload[4];
        if (dlc > 8 || len != 5 + dlc) return false;
        f = Frame{};
        f.id = raw & 0x1FFFFFFFu;
        f.extended = (raw & 0x80000000u) != 0;
        f.len = dlc;
        for (uint8_t i = 0; i < dlc; ++i) f.data[i] = payload[5 + i];
        f.source = Frame::SRC_VH_CAN;
        return true;
    }

    bool Record::asStatus(Status& s) const
    {
        if (type != T_STATUS || len != 19) return false;
        s.frames    = get32(payload);
        s.forwarded = get32(payload + 4);
        s.fps       = (uint16_t)((payload[8] << 8) | payload[9]);
        s.rxErr     = payload[10];
        s.mode      = payload[11];
        s.busErr    = get32(payload + 12);
        s.state     = payload[16];
        s.sim       = (uint16_t)((payload[17] << 8) | payload[18]);
        return true;
    }

    uint8_t encodeSim(uint16_t fps, uint8_t* out)
    {
        uint8_t p[2] = { (uint8_t)(fps >> 8), (uint8_t)fps };
        return seal(T_SIM, p, 2, out);
    }

    bool Record::asSim(uint16_t& fps) const
    {
        if (type != T_SIM || len != 2) return false;
        fps = (uint16_t)((payload[0] << 8) | payload[1]);
        return true;
    }

    bool Record::asFilter(FilterMode& mode, uint32_t* ids, uint8_t& n) const
    {
        if (type != T_FILTER || len < 2) return false;
        const uint8_t m = payload[0], cnt = payload[1];
        if (m > F_LIST_CHANGES || cnt > MAX_FILTER_IDS || len != 2 + 4 * cnt) return false;
        mode = (FilterMode)m;
        n = cnt;
        for (uint8_t i = 0; i < cnt; ++i) ids[i] = get32(payload + 2 + 4 * i);
        return true;
    }

    // ---- decoder ----------------------------------------------------------------
    bool Decoder::feed(uint8_t b, Record& out)
    {
        switch (_st) {
            case St::Sync:
                if (b == SYNC) _st = St::Type;
                else ++_resyncs;
                return false;
            case St::Type:
                _cur.type = (Type)b;
                _st = St::Len;
                return false;
            case St::Len:
                if (b > MAX_PAYLOAD) { _st = St::Sync; ++_resyncs; return false; }
                _cur.len = b;
                _have = 0;
                _st = b ? St::Payload : St::Crc;
                return false;
            case St::Payload:
                _cur.payload[_have++] = b;
                if (_have == _cur.len) _st = St::Crc;
                return false;
            case St::Crc: {
                _st = St::Sync;
                uint8_t buf[2 + MAX_PAYLOAD];
                buf[0] = (uint8_t)_cur.type;
                buf[1] = _cur.len;
                memcpy(buf + 2, _cur.payload, _cur.len);
                if (crc8(buf, (size_t)_cur.len + 2) != b) { ++_crcErrors; return false; }
                out = _cur;
                ++_records;
                return true;
            }
        }
        return false;
    }

    // ---- change filter ------------------------------------------------------------
    void ChangeFilter::setList(const uint32_t* ids, uint8_t n)
    {
        if (n > MAX_FILTER_IDS) n = MAX_FILTER_IDS;
        _listN = n;
        for (uint8_t i = 0; i < n; ++i) _list[i] = ids[i];
    }

    bool ChangeFilter::listed(uint32_t id) const
    {
        for (uint8_t i = 0; i < _listN; ++i) if (_list[i] == id) return true;
        return false;
    }

    void ChangeFilter::reset()
    {
        memset(_s, 0, sizeof(_s));
        _n = 0;
        _overflow = 0;
    }

    bool ChangeFilter::pass(const Frame& f, uint32_t nowMs)
    {
        if ((_mode == F_LIST || _mode == F_LIST_CHANGES) && !listed(f.id)) return false;
        if (_mode == F_ALL || _mode == F_LIST) return true;

        // Changes only. Find or add the id's slot.
        Slot* s = nullptr;
        for (uint8_t i = 0; i < _n; ++i) if (_s[i].used && _s[i].id == f.id) { s = &_s[i]; break; }
        if (!s) {
            // An unknown id always passes. With the table full it passes on
            // every frame: overflow costs traffic, never silence.
            if (_n >= MAX_IDS) { ++_overflow; return true; }
            s = &_s[_n++];
            s->used = true; s->id = f.id; s->len = f.len;
            memcpy(s->data, f.data, 8);
            s->sentMs = nowMs;
            return true;
        }
        const bool changed = s->len != f.len || memcmp(s->data, f.data, 8) != 0;
        const bool stale   = (uint32_t)(nowMs - s->sentMs) >= REFRESH_MS;
        if (!changed && !stale) return false;
        s->len = f.len;
        memcpy(s->data, f.data, 8);
        s->sentMs = nowMs;
        return true;
    }
}
