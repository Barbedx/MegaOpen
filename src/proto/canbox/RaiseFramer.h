#pragma once
#include <stdint.h>

// Raise / "canbox" serial framing as Android head units expect it:
//
//   [0x2E][cmd][len][payload ...][cs]
//   cs = (cmd + len + sum(payload)) XOR 0xFF
//   38400 8N1, fire and forget — the unit never acknowledges.
//
// Portable and allocation-free; covered by golden-vector tests.
namespace Canbox
{
    static constexpr uint8_t SOF = 0x2E;
    // RX is sized for undocumented inbound text (DUDU now-playing, 0x7D family),
    // not just the short documented status frames.
    static constexpr uint8_t MAX_PAYLOAD = 32;    // what WE build
    static constexpr uint8_t MAX_FRAME = MAX_PAYLOAD + 4;
    static constexpr uint16_t MAX_RX_PAYLOAD = 192;   // what we are willing to RECEIVE

    uint8_t checksum(uint8_t cmd, uint8_t len, const uint8_t* payload);

    // Writes len+4 bytes into out; returns the total frame length, or 0 if the
    // payload is too long.
    uint8_t buildFrame(uint8_t cmd, const uint8_t* payload, uint8_t len, uint8_t* out);

    // Incremental parser for what the head unit sends back to the canbox.
    class RxParser {
    public:
        using FrameCb = void (*)(uint8_t cmd, const uint8_t* payload, uint8_t len, void* ctx);
        using JunkCb  = void (*)(uint8_t byte, void* ctx);   // bytes outside any frame
        // Fired as soon as a frame is complete, before the checksum check and
        // dispatch: the spec allows 10 ms to ack and a 12-byte frame takes ~4 ms.
        using AckCb   = void (*)(void* ctx);

        void onFrame(FrameCb cb, void* ctx) { _cb = cb; _ctx = ctx; }
        void onJunk(JunkCb cb, void* ctx) { _junk = cb; _jctx = ctx; }
        void onAck(AckCb cb, void* ctx) { _ack = cb; _actx = ctx; }
        void feed(uint8_t b);

        uint32_t frames() const { return _frames; }
        uint32_t badChecksums() const { return _bad; }
        uint32_t truncated() const { return _truncated; }
        // Bytes outside any frame. Bytes here and no frames = the wires are
        // right but the speed or the dialect is not.
        uint32_t junkBytes() const { return _junkBytes; }
        // Declared payload length of the frame just delivered — larger than the
        // buffered length when the frame was longer than MAX_RX_PAYLOAD.
        uint16_t lastTotalLen() const { return _lastTotal; }
        // Inside a frame: its ack will be owed the moment it ends, so the
        // vehicle lane holds off (sinks/UartLanes.h).
        bool midFrame() const { return _s != S::Sof; }

        // Abandon a frame in progress: the head unit stopped mid-frame (reset,
        // power-down) or a corrupt length byte promised more than will come.
        // Without it the parser swallows the next real frames as payload and
        // midFrame() holds the reply and vehicle lanes shut. The owner calls
        // it when the line has been silent mid-frame for too long; no ack.
        void reset()
        {
            if (_s != S::Sof) ++_desyncs;
            _s = S::Sof;
        }
        // Frames abandoned by reset().
        uint32_t desyncs() const { return _desyncs; }

    private:
        enum class S : uint8_t { Sof, Cmd, Len, Payload, Cs };
        S        _s = S::Sof;
        uint8_t  _cmd = 0;
        uint16_t _len = 0, _idx = 0, _lastTotal = 0;
        uint8_t  _sum = 0;               // runs over ALL payload bytes, stored or not
        uint8_t  _buf[MAX_RX_PAYLOAD];
        FrameCb  _cb = nullptr;   void* _ctx = nullptr;
        JunkCb   _junk = nullptr; void* _jctx = nullptr;
        AckCb    _ack = nullptr;  void* _actx = nullptr;
        uint32_t _frames = 0, _bad = 0, _truncated = 0, _desyncs = 0, _junkBytes = 0;
    };
}
