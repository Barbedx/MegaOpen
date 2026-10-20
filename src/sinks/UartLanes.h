#pragma once
#include <stdint.h>

// Who may put bytes into the head-unit UART, and when (docs/design/event-tasks.md
// §3.5). Portable: UartTx asks these, the host tests prove them.
//
// The port has no TX ring buffer (Serial1 is begun without one), so whatever is
// in the 128-byte hardware FIFO is all that stands between a new frame and the
// wire. That is the whole policy: a vehicle frame is written only into an EMPTY
// FIFO, so the most an ack or a key can ever find in front of it is one vehicle
// frame. The head unit allows 10 ms for an ack; at 38400 baud a byte is 260 us,
// so the 20-byte longest frame of the Rav4 table is 5.2 ms of it.
//
// Keys skip the gate but not the budget: a burst of them (a macro replaying
// held taps, a batch of panel keys) goes into the FIFO only while it leaves at
// most KEY_BUDGET bytes in front of the next ack; UartTx holds the rest for the
// keys task's next pass. So an ack finds at most a vehicle frame's worth of
// keys in front of it, too.
//
// Never add a TX ring "for throughput": a ring is exactly a queue of telemetry
// in front of the next key.
namespace Sinks
{
    namespace UartLanes
    {
        constexpr uint16_t FIFO_BYTES  = 128;    // ESP32, S3 and C3 UART FIFO
        constexpr uint32_t US_PER_BYTE = 260;    // 10 bits at 38400 baud

        // Frames longer than this (a hand-made raw frame can be 36 B) would
        // eat more than 6.2 ms of the ack window, so they also wait for the
        // head unit to have been quiet for QUIET_MS. Every Rav4 message fits
        // under it; test_uartlanes measures that.
        constexpr uint8_t  QUIET_LEN_MAX = 24;
        constexpr uint32_t QUIET_MS      = 20;

        // Highest priority first. Raw (web, macros) rides the Vehicle lane.
        enum class Lane : uint8_t { Ack = 0, Key = 1, Reply = 2, Vehicle = 3 };
        constexpr uint8_t LANES = 4;

        inline const char* name(Lane l)
        {
            switch (l) {
                case Lane::Ack:     return "ack";
                case Lane::Key:     return "key";
                case Lane::Reply:   return "reply";
                case Lane::Vehicle: return "vehicle";
            }
            return "?";
        }

        // Bytes still in the FIFO, from HardwareSerial::availableForWrite().
        inline uint16_t inflight(int availableForWrite)
        {
            if (availableForWrite <= 0) return FIFO_BYTES;
            if (availableForWrite >= (int)FIFO_BYTES) return 0;
            return (uint16_t)(FIFO_BYTES - availableForWrite);
        }

        // How long `bytes` already in the FIFO hold up whatever comes next.
        inline uint32_t wireUs(uint32_t bytes) { return bytes * US_PER_BYTE; }

        // Keys in the FIFO in front of the next ack: the 6.2 ms a vehicle
        // frame may take. A key frame goes in when it fits under it, or into
        // an empty FIFO whatever its length. The keys task refills every
        // millisecond (3.8 bytes) while it holds frames back, so the FIFO
        // never runs dry and a burst reaches the wire no later than before.
        constexpr uint16_t KEY_BUDGET = QUIET_LEN_MAX;

        inline bool keyFits(uint16_t inflightBytes, uint8_t len)
        {
            return inflightBytes == 0 || (uint32_t)inflightBytes + len <= KEY_BUDGET;
        }

        // May a frame of `len` bytes go into the FIFO now? Acks and keys always
        // may (keys are budgeted by keyFits() in UartTx, not here). Everything else needs an empty FIFO and the head unit not in the
        // middle of sending us a frame, whose ack we are about to owe; a long
        // one also needs the link quiet (see QUIET_LEN_MAX).
        inline bool mayWrite(Lane l, uint16_t inflightBytes, bool rxMidFrame,
                             uint8_t len, uint32_t msSinceRx)
        {
            if (l == Lane::Ack || l == Lane::Key) return true;
            if (inflightBytes != 0 || rxMidFrame) return false;
            if (len > QUIET_LEN_MAX && msSinceRx < QUIET_MS) return false;
            return true;
        }

        // The lane to serve first among those with something waiting (bit per
        // lane, bit 0 = Ack). -1 when none.
        inline int8_t pick(uint8_t pendingMask)
        {
            for (uint8_t i = 0; i < LANES; ++i)
                if (pendingMask & (1u << i)) return (int8_t)i;
            return -1;
        }
    }
}
