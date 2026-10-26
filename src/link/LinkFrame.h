#pragma once
#include <stddef.h>
#include <stdint.h>

// The USB link's frames (docs/design/usb-link.md): magic, type, flags, chan,
// len, payload, CRC-16. Pure: host-tested in test/test_link.
namespace Link
{
    constexpr uint8_t  MAGIC0 = 0x4D;   // 'M'
    constexpr uint8_t  MAGIC1 = 0x4F;   // 'O'
    constexpr size_t   HEAD = 8;
    constexpr size_t   MAX_PAYLOAD = 1024;
    constexpr size_t   MAX_FRAME = HEAD + MAX_PAYLOAD + 2;
    constexpr uint8_t  FIN = 0x01;

    // The high bit is the direction: set, board to app.
    enum Type : uint8_t {
        HELLO      = 0x01,
        PING       = 0x02,
        HTTP_REQ   = 0x10,
        HTTP_CLOSE = 0x11,
        NET_HEAD   = 0x40,   // the app's internet: the answer's status, length, URL
        NET_DATA   = 0x41,   //   its body, FIN at the end
        NET_FAIL   = 0x42,   //   or why there is none
        HELLO_ACK  = 0x81,
        PONG       = 0x82,
        HTTP_RESP  = 0x90,
        HTTP_FAIL  = 0x91,
        EVENT      = 0xA0,
        NET_REQ    = 0xC0,   // the board's request: JSON head, FIN when no body
        NET_BODY   = 0xC1,   //   its body, FIN on the last frame
        NET_ACK    = 0xC2,   //   bytes taken so far (u32): the app's credit
        NET_CANCEL = 0xC3,   //   stop sending
    };

    // CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), as Kotlin computes it.
    uint16_t crc16(const uint8_t* d, size_t n, uint16_t crc = 0xFFFF);

    // The whole frame into out (MAX_FRAME is always enough); its length, or 0
    // when len is over MAX_PAYLOAD.
    size_t encode(uint8_t type, uint8_t flags, uint16_t chan, const uint8_t* payload, size_t len,
                  uint8_t* out);

    struct Frame {
        uint8_t        type;
        uint8_t        flags;
        uint16_t       chan;
        const uint8_t* payload;   // valid until the next feed()
        uint16_t       len;
    };

    // Bytes in, frames out. A bad magic, length or CRC drops one byte and
    // searches for the magic again, so a frame cut by a replug costs only
    // itself.
    class Parser {
    public:
        typedef void (*OnFrame)(const Frame& f, void* ctx);
        void feed(const uint8_t* d, size_t n, OnFrame cb, void* ctx);
        uint32_t bad() const { return _bad; }
        void reset() { _n = 0; }

    private:
        // Hand out every complete frame at the front of the buffer, drop
        // garbage; leave a partial one.
        void drain(OnFrame cb, void* ctx);
        // Where a whole, CRC-good frame starts at or after `from`; 0 if none.
        size_t wholeAfter(size_t from) const;
        uint8_t  _buf[MAX_FRAME];
        size_t   _n = 0;
        uint32_t _bad = 0;
    };
}
