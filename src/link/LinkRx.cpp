#include "LinkRx.h"
#if FEATURE_LINK

namespace Link
{
    void Rx::begin(HardwareSerial& port, int8_t rxPin, int8_t txPin)
    {
        _port = &port;
        _port->begin(BAUD, SERIAL_8N1, rxPin, txPin);
        // V keeps nothing across a boot: greet it, then resend the filter.
        uint8_t rec[MAX_RECORD];
        _port->write(rec, encodeHello(1, rec));
        _filterOwed = true;
    }

    void Rx::setSim(uint16_t fps)
    {
        // Written by drain(), on the task that owns the port.
        _simReq.store((int32_t)fps);
    }

    void Rx::setFilter(FilterMode mode, const uint32_t* ids, uint8_t n)
    {
        if (n > MAX_FILTER_IDS) n = MAX_FILTER_IDS;
        _lock.lock();
        _mode = mode;
        _n = n;
        for (uint8_t i = 0; i < n; ++i) _ids[i] = ids[i];
        _lock.unlock();
        _filterOwed = true;
    }

    FilterMode Rx::filterMode() const
    {
        _lock.lock();
        const FilterMode m = _mode;
        _lock.unlock();
        return m;
    }

    uint8_t Rx::filterIds(uint32_t* out) const
    {
        _lock.lock();
        const uint8_t n = _n;
        for (uint8_t i = 0; i < n; ++i) out[i] = _ids[i];
        _lock.unlock();
        return n;
    }

    size_t Rx::drain(uint32_t nowMs, Frame* out, size_t max)
    {
        if (!_port) return 0;

        size_t got = 0;
        Record r;
        // Bounded in bytes and in frames, so a burst cannot hold the owner;
        // the decoder keeps a half-read record for the next pass.
        for (uint16_t n = 0; n < 1024 && got < max && _port->available(); ++n) {
            if (!_dec.feed((uint8_t)_port->read(), r)) continue;
            Frame f;
            if (r.asCan(f)) {
                ++_frames; ++_fpsCount;
                out[got++] = f;
            } else if (r.asStatus(_status)) {
                _statusAt = nowMs;
            } else if (r.type == T_HELLO && r.len >= 1) {
                _version = r.payload[0];
                _statusAt = nowMs;
                // V has just booted (or answered ours): it knows no filter.
                _filterOwed = true;
            }
        }

        if ((uint32_t)(nowMs - _fpsAt) >= 1000) {
            _fpsAt = nowMs;
            _fps = (uint16_t)(_fpsCount > 65535 ? 65535 : _fpsCount);
            _fpsCount = 0;
        }

        // The web's sim request, written here so only this task writes the port.
        const int32_t sim = _simReq.exchange(-1);
        if (sim >= 0) {
            uint8_t rec[MAX_RECORD];
            _port->write(rec, encodeSim((uint16_t)sim, rec));
        }

        // Send the filter when owed and V is up; re-greet a silent V every 2 s
        // (its hello reply makes the filter owed again).
        if (_filterOwed && _statusAt) {
            // Cleared before the copy: a setFilter() landing during it owes
            // the filter again rather than being lost.
            _filterOwed = false;
            uint32_t ids[MAX_FILTER_IDS];
            const uint8_t n = filterIds(ids);
            const FilterMode mode = filterMode();
            uint8_t rec[MAX_RECORD];
            _port->write(rec, encodeFilter(mode, ids, n, rec));
        } else if (!alive(nowMs) && (uint32_t)(nowMs - _helloAt) >= 2000) {
            _helloAt = nowMs;
            uint8_t rec[MAX_RECORD];
            _port->write(rec, encodeHello(1, rec));
        }
        return got;
    }
}
#endif
