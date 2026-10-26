#include "LinkFrame.h"
#include <string.h>

namespace Link
{
    uint16_t crc16(const uint8_t* d, size_t n, uint16_t crc)
    {
        for (size_t i = 0; i < n; ++i) {
            crc ^= (uint16_t)d[i] << 8;
            for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
        return crc;
    }

    size_t encode(uint8_t type, uint8_t flags, uint16_t chan, const uint8_t* payload, size_t len, uint8_t* out)
    {
        if (len > MAX_PAYLOAD) return 0;
        out[0] = MAGIC0;
        out[1] = MAGIC1;
        out[2] = type;
        out[3] = flags;
        out[4] = (uint8_t)chan;
        out[5] = (uint8_t)(chan >> 8);
        out[6] = (uint8_t)len;
        out[7] = (uint8_t)(len >> 8);
        if (len) memcpy(out + HEAD, payload, len);
        const uint16_t c = crc16(out + 2, HEAD - 2 + len);
        out[HEAD + len] = (uint8_t)c;
        out[HEAD + len + 1] = (uint8_t)(c >> 8);
        return HEAD + len + 2;
    }

    void Parser::feed(const uint8_t* d, size_t n, OnFrame cb, void* ctx)
    {
        while (n) {
            const size_t take = n < sizeof(_buf) - _n ? n : sizeof(_buf) - _n;
            memcpy(_buf + _n, d, take);
            _n += take;
            d += take;
            n -= take;
            drain(cb, ctx);
        }
    }

    size_t Parser::wholeAfter(size_t from) const
    {
        for (size_t i = from; i + HEAD + 2 <= _n; ++i) {
            if (_buf[i] != MAGIC0 || _buf[i + 1] != MAGIC1) continue;
            const size_t len = (size_t)_buf[i + 6] | ((size_t)_buf[i + 7] << 8);
            if (len > MAX_PAYLOAD || i + HEAD + len + 2 > _n) continue;
            const uint16_t want = (uint16_t)_buf[i + HEAD + len] | ((uint16_t)_buf[i + HEAD + len + 1] << 8);
            if (crc16(_buf + i + 2, HEAD - 2 + len) == want) return i;
        }
        return 0;
    }

    void Parser::drain(OnFrame cb, void* ctx)
    {
        size_t at = 0;
        for (;;) {
            // Find the magic.
            while (at < _n && _buf[at] != MAGIC0) ++at, ++_bad;
            if (_n - at < 2) break;
            if (_buf[at + 1] != MAGIC1) { ++at; ++_bad; continue; }
            if (_n - at < HEAD) break;
            const uint8_t* h = _buf + at;
            const size_t len = (size_t)h[6] | ((size_t)h[7] << 8);
            if (len > MAX_PAYLOAD) { ++at; ++_bad; continue; }
            if (_n - at < HEAD + len + 2) {
                // Not all here yet. A frame cut short (a replug, a writer that
                // gave up) has a length that swallows the frames after it until
                // that many bytes came: on a quiet link, minutes. A whole,
                // CRC-good frame further on means this one never ends.
                const size_t nx = wholeAfter(at + 1);
                if (nx) { _bad += (uint32_t)(nx - at); at = nx; continue; }
                break;
            }
            const uint16_t want = (uint16_t)h[HEAD + len] | ((uint16_t)h[HEAD + len + 1] << 8);
            if (crc16(h + 2, HEAD - 2 + len) != want) { ++at; ++_bad; continue; }
            Frame f;
            f.type = h[2];
            f.flags = h[3];
            f.chan = (uint16_t)(h[4] | (h[5] << 8));
            f.payload = h + HEAD;
            f.len = (uint16_t)len;
            cb(f, ctx);
            at += HEAD + len + 2;
        }
        if (at) {
            memmove(_buf, _buf + at, _n - at);
            _n -= at;
        }
    }
}
