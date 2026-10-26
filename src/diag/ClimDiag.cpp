#include "ClimDiag.h"
#include <string.h>

namespace Diag
{
    bool allowed(const uint8_t* req, uint8_t n)
    {
        if (!req || n != 2) return false;
        const uint16_t r = (uint16_t)(req[0] << 8 | req[1]);
        switch (r) {
            case 0x2180: case 0x21C0: case 0x21C1: case 0x21C2: case 0x2190: case 0x21A0:
            case 0x10C0: case 0x3E01: case 0x1081:
                return true;
            default:
                return false;
        }
    }

    bool Exchange::start(const uint8_t* req, uint8_t n, uint32_t nowMs)
    {
        if (_st == Status::Waiting) return false;
        _nrc = 0;
        _have = _want = _seq = 0;
        if (!allowed(req, n)) { done(Status::Refused); return false; }
        uint8_t f[8];
        f[0] = n;                       // single frame: PCI 0x0N
        memcpy(f + 1, req, n);
        _sid = req[0];
        if (!_send || !_send(CLIM_REQ, f, (uint8_t)(n + 1), _ctx)) { done(Status::SendFailed); return false; }
        _lastMs = nowMs;
        _limitMs = REPLY_TIMEOUT_MS;
        _st = Status::Waiting;
        return true;
    }

    void Exchange::onFrame(uint32_t id, const uint8_t* d, uint8_t len, uint32_t nowMs)
    {
        if (_st != Status::Waiting || id != CLIM_RESP || !d || len < 1) return;
        const uint8_t pci = d[0] >> 4;
        if (pci == 0x0) {                                    // single frame
            const uint8_t n = d[0] & 0x0F;
            if (_want || n < 1 || n > 7 || n + 1 > len) { done(Status::Bad); return; }
            memcpy(_buf, d + 1, n);
            _have = _want = n;
        } else if (pci == 0x1) {                             // first frame
            if (_want || len < 8) { done(Status::Bad); return; }
            const uint16_t total = (uint16_t)(((d[0] & 0x0F) << 8) | d[1]);
            if (total < 8 || total > REPLY_MAX) { done(Status::Bad); return; }
            _want = (uint8_t)total;
            memcpy(_buf, d + 2, 6);
            _have = 6;
            _seq = 1;
            // Flow control, the only other frame out: send everything, no gap.
            static const uint8_t fc[3] = {0x30, 0x00, 0x00};
            if (!_send || !_send(CLIM_REQ, fc, sizeof(fc), _ctx)) { done(Status::SendFailed); return; }
        } else if (pci == 0x2) {                             // consecutive frame
            if (!_want || _have >= _want || (d[0] & 0x0F) != (_seq & 0x0F)) { done(Status::Bad); return; }
            ++_seq;
            uint8_t k = (uint8_t)(len - 1);
            if (k > _want - _have) k = (uint8_t)(_want - _have);
            memcpy(_buf + _have, d + 1, k);
            _have = (uint8_t)(_have + k);
        } else {
            return;                                          // 0x3: not ours to read
        }
        _lastMs = nowMs;
        if (_have < _want) return;

        if (_buf[0] == 0x7F) {
            if (_have >= 3 && _buf[1] == _sid) {
                // 78: response pending, the reply still comes; wait on.
                if (_buf[2] == 0x78) { _have = _want = 0; _limitMs = PENDING_TIMEOUT_MS; return; }
                _nrc = _buf[2];
                done(Status::Negative);
            } else {
                done(Status::Bad);
            }
        } else if (_buf[0] == (uint8_t)(_sid + 0x40)) {
            done(Status::Ok);
        } else {
            done(Status::Bad);
        }
    }

    void Exchange::tick(uint32_t nowMs)
    {
        if (_st == Status::Waiting && (uint32_t)(nowMs - _lastMs) >= _limitMs) done(Status::Timeout);
    }

    const char* Exchange::statusId(Status s)
    {
        switch (s) {
            case Status::Idle:       return "idle";
            case Status::Waiting:    return "waiting";
            case Status::Ok:         return "ok";
            case Status::Negative:   return "negative";
            case Status::Timeout:    return "timeout";
            case Status::Refused:    return "refused";
            case Status::SendFailed: return "send_failed";
            case Status::Bad:        return "bad";
        }
        return "?";
    }

    bool decode21C1(const uint8_t* r, uint8_t n, ClimValues& v)
    {
        // DDT byte k is r[k - 1].
        if (!r || n < 10 || r[0] != 0x61 || r[1] != 0xC1) return false;
        v.outsideC = (int16_t)(r[3] - 40);
        v.coolantC = (int16_t)(r[5] - 40);
        v.evapX10  = (int16_t)(r[7] * 5 - 400);
        v.cabinX10 = (int16_t)(r[8] * 5 - 400);
        v.sunWm2   = (uint16_t)(r[9] * 3);
        v.battX100 = n >= 12 ? (uint16_t)((r[11] * 78 + 5) / 10) : 0;
        return true;
    }

    bool decode2190(const uint8_t* r, uint8_t n, Clim2190& v)
    {
        if (!r || n < 5 || r[0] != 0x61 || r[1] != 0x90) return false;
        v.recirc = (uint8_t)(r[2] >> 5);
        v.setX10 = (uint16_t)(((r[2] & 0x1F) + 30) * 5);
        v.dist   = (uint8_t)(r[3] >> 4);
        v.blower = (uint8_t)(r[3] & 0x0F);
        v.ac     = (uint8_t)(r[4] & 0x0F);
        return true;
    }
}
