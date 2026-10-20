#pragma once
#include "../Features.h"
#if FEATURE_LINK
#include "../bus/IVehicleSource.h"
#include "LinkRx.h"

// Board V's frames (the C3 on the OBD2 CAN, over UART2 on D26/D27) as one of
// the car task's sources (docs/design/event-tasks.md, Step 5). The port is opened
// and V greeted in setup() (Link::Rx::begin), before the car task exists; from
// then on only the car task touches it, through drain().
class LinkSource : public IVehicleSource {
public:
    explicit LinkSource(Link::Rx& rx) : _rx(rx) {}

    const char* name() const override { return "link"; }
    bool        begin() override { return true; }
    size_t      drain(Frame* out, size_t max, uint32_t nowMs) override
    {
        return _rx.drain(nowMs, out, max);
    }
    Stats stats() const override
    {
        Stats s;
        s.frames  = _rx.frames();
        // A record lost on the wire: a CRC failure or a resync.
        s.dropped = _rx.crcErrors() + _rx.resyncs();
        return s;
    }

private:
    Link::Rx& _rx;
};
#endif
