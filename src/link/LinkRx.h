#pragma once
#include "../Features.h"
#if FEATURE_LINK
#include <Arduino.h>
#include <atomic>
#include "LinkProto.h"
#include "../utils/Locks.h"

// The brain's end of the link: a UART, a decoder, and the CAN frames board V
// forwards, tagged SRC_VH_CAN. LinkSource hands them to the car task.
//
// Drained by the car task every 2 ms (docs/design/event-tasks.md, Step 5): 1500
// frames/s is 20 kB/s, so the 256-byte UART buffer holds 12 ms. A dropped byte
// shows as `resyncs`.
//
// ONE OWNER of the port after begin(): the task that calls drain(). httpd's
// setFilter() and setSim() only leave a request, which drain() writes to the
// port on its next pass; nothing off the owner task writes Serial2.
namespace Link
{
    class Rx {
    public:
        // setup(), before the owner task starts: opens the port and greets V.
        void begin(HardwareSerial& port, int8_t rxPin, int8_t txPin);
        // The owner: up to `max` CAN frames into `out`, at most 1024 bytes read;
        // status and hello records are handled on the way. How many frames.
        size_t drain(uint32_t nowMs, Frame* out, size_t max);

        // Any task. Push a filter down to board V (no NVS there; re-sent on every
        // boot of either end). The caller persists it.
        void setFilter(FilterMode mode, const uint32_t* ids, uint8_t n);
        // Any task. Ask board V to simulate `fps` frames/s, 0 = stop. The actual
        // rate comes back in status().sim.
        void setSim(uint16_t fps);
        FilterMode filterMode() const;
        uint8_t    filterIds(uint32_t* out) const;   // out[MAX_FILTER_IDS]

        // What we know about the far end.
        bool     alive(uint32_t nowMs) const { return _statusAt && (uint32_t)(nowMs - _statusAt) < 3500; }
        const Status& status() const { return _status; }
        uint32_t frames() const { return _frames; }     // CAN records received
        uint16_t fps() const { return _fps; }
        uint32_t crcErrors() const { return _dec.crcErrors(); }
        uint32_t resyncs() const { return _dec.resyncs(); }
        uint8_t  version() const { return _version; }

    private:
        HardwareSerial* _port = nullptr;
        Decoder  _dec;
        Status   _status{};
        uint32_t _statusAt = 0;
        uint8_t  _version = 0;
        uint32_t _frames = 0, _fpsCount = 0, _fpsAt = 0;
        uint16_t _fps = 0;
        // The filter: written by setFilter() on any task, read by drain(), so
        // copied in and out under the spinlock.
        mutable Locks::Spin _lock;
        FilterMode _mode = F_ALL;
        uint32_t _ids[MAX_FILTER_IDS] = {};
        uint8_t  _n = 0;
        uint32_t _helloAt = 0;
        std::atomic<bool>    _filterOwed{false};
        // A sim rate to send, or -1.
        std::atomic<int32_t> _simReq{-1};
    };
}
#endif
