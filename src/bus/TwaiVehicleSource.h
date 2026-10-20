#pragma once
#include <atomic>
#include "IVehicleSource.h"

// The car's frames from the TWAI controller in the vehicle role (env:direct's
// OBD tap). Portable: a ring and two counters, native-tested.
//
// The TWAI driver stays with the task that owns it — the loop task, where the
// CAN bench ops, the bus-off recovery and the wait-for-a-frame sleep all live —
// so this source does not read the controller. The owner's RX handler push()es
// each frame here; the car task drain()s them. One producer, one consumer: a
// lock-free ring, and a frame that finds it full is dropped and counted, never
// waited for.
class TwaiVehicleSource : public IVehicleSource {
public:
    // 64 frames is 40 ms at 1500 frames/s against a 2 ms poll.
    static constexpr uint16_t DEPTH = 64;

    static TwaiVehicleSource& instance();

    // The TWAI owner's RX handler (an RxHandler). Any one task, never blocks.
    static void push(const Frame& f, void* ctx);
    bool put(const Frame& f);

    const char* name() const override { return "twai"; }
    bool        begin() override { return true; }
    size_t      drain(Frame* out, size_t max, uint32_t nowMs) override;
    // The vehicle role runs the TWAI at a fixed 500k (setup() in main.cpp).
    uint32_t    bitrate() const override { return 500000; }
    Stats       stats() const override;

private:
    Frame                 _r[DEPTH];
    std::atomic<uint16_t> _head{0};   // the producer's
    std::atomic<uint16_t> _tail{0};   // the consumer's
    std::atomic<uint32_t> _in{0}, _drops{0};
    uint32_t              _out = 0;
};
