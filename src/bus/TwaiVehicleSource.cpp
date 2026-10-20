#include "TwaiVehicleSource.h"

TwaiVehicleSource& TwaiVehicleSource::instance()
{
    static TwaiVehicleSource s;
    return s;
}

void TwaiVehicleSource::push(const Frame& f, void*)
{
    instance().put(f);
}

bool TwaiVehicleSource::put(const Frame& f)
{
    const uint16_t h = _head.load(std::memory_order_relaxed);
    const uint16_t next = (uint16_t)((h + 1) % DEPTH);
    if (next == _tail.load(std::memory_order_acquire)) {
        _drops.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    _r[h] = f;
    _r[h].source = Frame::SRC_VH_CAN;
    _head.store(next, std::memory_order_release);
    _in.fetch_add(1, std::memory_order_relaxed);
    return true;
}

size_t TwaiVehicleSource::drain(Frame* out, size_t max, uint32_t)
{
    size_t n = 0;
    uint16_t t = _tail.load(std::memory_order_relaxed);
    const uint16_t h = _head.load(std::memory_order_acquire);
    while (n < max && t != h) {
        out[n++] = _r[t];
        t = (uint16_t)((t + 1) % DEPTH);
    }
    _tail.store(t, std::memory_order_release);
    _out += (uint32_t)n;
    return n;
}

IVehicleSource::Stats TwaiVehicleSource::stats() const
{
    Stats s;
    s.frames  = _out;
    s.dropped = _drops.load(std::memory_order_relaxed);
    return s;
}
