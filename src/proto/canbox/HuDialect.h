#pragma once
#include <stdint.h>
#include <atomic>

// Which canbox profile the head unit speaks, told from what it sends us.
//
// The Raise profiles are disjoint inbound (docs/raise-protocol.md §2b): a
// Toyota-profile unit sends 0x81 / 0x83 / 0x84 (and the DUDU's 0x99 text and
// 0x9D clock), never 0xC0..0xC6; a VW-profile unit sends 0xC0..0xC6 and
// 0xA0 / 0xA2, never those. 0x90 (a request) and 0xC7 (seen from the DUDU on
// the Toyota profile) belong to neither. This firmware speaks Toyota RAV4
// only, so a VW-speaking unit is wired right but set to the wrong profile.
//
// A 0xF3 byte between frames is the unit's NACK "command not supported" for a
// frame of ours: counted, as a hint, not a verdict.
//
// Counters only, relaxed atomics: the hu task writes, httpd reads.
// Pure (no Arduino): host-tested in test/test_humedia.
namespace Canbox
{
    class HuDialect {
    public:
        enum class Kind : uint8_t { Unknown, Toyota, Vw };

        // VW frames before a verdict: one stray byte pattern is not a profile.
        static constexpr uint32_t VW_MIN = 3;

        static bool toyotaOnly(uint8_t cmd)
        {
            return cmd == 0x81 || cmd == 0x83 || cmd == 0x84 || cmd == 0x99 || cmd == 0x9D;
        }
        static bool vwOnly(uint8_t cmd)
        {
            return cmd == 0xA0 || cmd == 0xA2 || (cmd >= 0xC0 && cmd <= 0xC6);
        }

        // Every inbound frame with a good checksum.
        void onFrame(uint8_t cmd)
        {
            if (toyotaOnly(cmd)) _toyota.fetch_add(1, std::memory_order_relaxed);
            else if (vwOnly(cmd)) _vw.fetch_add(1, std::memory_order_relaxed);
        }
        // Every byte outside a frame.
        void onJunk(uint8_t b)
        {
            if (b == 0xF3) _nack.fetch_add(1, std::memory_order_relaxed);
        }

        // VW once it has sent VW_MIN of its own frames and more of them than
        // Toyota ones; Toyota once it has sent any Toyota frame; else unknown.
        Kind kind() const
        {
            const uint32_t t = toyota(), v = vw();
            if (v >= VW_MIN && v > t) return Kind::Vw;
            if (t) return Kind::Toyota;
            return Kind::Unknown;
        }
        static const char* name(Kind k)
        {
            return k == Kind::Toyota ? "toyota" : k == Kind::Vw ? "vw" : "unknown";
        }

        uint32_t toyota() const       { return _toyota.load(std::memory_order_relaxed); }
        uint32_t vw() const           { return _vw.load(std::memory_order_relaxed); }
        uint32_t notSupported() const { return _nack.load(std::memory_order_relaxed); }

    private:
        std::atomic<uint32_t> _toyota{0}, _vw{0}, _nack{0};
    };
}
