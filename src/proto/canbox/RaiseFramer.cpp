#include "RaiseFramer.h"

namespace Canbox
{
    uint8_t checksum(uint8_t cmd, uint8_t len, const uint8_t* payload)
    {
        uint8_t cs = (uint8_t)(cmd + len);
        for (uint8_t i = 0; i < len; ++i) cs = (uint8_t)(cs + payload[i]);
        return (uint8_t)(cs ^ 0xFF);
    }

    uint8_t buildFrame(uint8_t cmd, const uint8_t* payload, uint8_t len, uint8_t* out)
    {
        if (!out || len > MAX_PAYLOAD) return 0;
        out[0] = SOF;
        out[1] = cmd;
        out[2] = len;
        for (uint8_t i = 0; i < len; ++i) out[3 + i] = payload[i];
        out[3 + len] = checksum(cmd, len, payload);
        return (uint8_t)(len + 4);
    }

    void RxParser::feed(uint8_t b)
    {
        switch (_s) {
            case S::Sof:
                if (b == SOF) _s = S::Cmd;
                else {
                    ++_junkBytes;
                    if (_junk) _junk(b, _jctx);
                }
                break;

            case S::Cmd:
                _cmd = b;
                _s = S::Len;
                break;

            case S::Len:
                _len = b;
                _idx = 0;
                _sum = 0;
                _s = _len ? S::Payload : S::Cs;
                break;

            case S::Payload:
                // Consume exactly the declared length even past the buffer;
                // hunting for 0x2E would resync inside the payload.
                _sum = (uint8_t)(_sum + b);
                if (_idx < MAX_RX_PAYLOAD) _buf[_idx] = b;
                ++_idx;
                if (_idx >= _len) _s = S::Cs;
                break;

            case S::Cs:
                // ACK unconditionally, even on a bad checksum: NACKs and silence
                // both lead to the spec's three-strike "stop transmitting".
                if (_ack) _ack(_actx);
                {
                    // The running sum covers bytes too long to store.
                    const uint8_t want = (uint8_t)(((uint8_t)(_cmd + (uint8_t)_len) + _sum) ^ 0xFF);
                    _lastTotal = _len;
                    if (b == want) {
                        ++_frames;
                        const uint16_t stored = _len < MAX_RX_PAYLOAD ? _len : MAX_RX_PAYLOAD;
                        if (_len > MAX_RX_PAYLOAD) ++_truncated;
                        if (_cb) _cb(_cmd, _buf, (uint8_t)(stored > 255 ? 255 : stored), _ctx);
                    } else {
                        ++_bad;
                    }
                }
                _s = S::Sof;
                break;
        }
    }
}
