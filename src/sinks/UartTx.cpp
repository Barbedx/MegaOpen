#include "UartTx.h"
#include <esp_timer.h>
#include <string.h>

void UartTx::begin(HardwareSerial* port)
{
    _port = port;
    _lock = xSemaphoreCreateMutex();
}

// What the FIFO still holds. Serial1 has no TX ring, so availableForWrite() is
// the FIFO's free space (sinks/UartLanes.h).
uint16_t UartTx::inflightNow()
{
    return Sinks::UartLanes::inflight(_port->availableForWrite());
}

// The first frame of the key being measured: what was in front of it, and
// when it was accepted. For a held frame `ahead` also counts the held bytes
// before it, which reach the wire first.
void UartTx::stampKey(uint16_t ahead)
{
    if (!_stampArmed) return;
    _stampInflight = ahead;
    _stampAtUs = (uint32_t)esp_timer_get_time();
    _stampArmed = false;
    _stampTaken = true;
}

uint8_t UartTx::writeHeldLocked(uint16_t& inflight, bool force)
{
    uint8_t k = 0;
    while (k < _kqN) {
        const KeyFrame& f = _kq[(uint8_t)((_kqHead + k) % KEY_HOLD)];
        if (!force && !Sinks::UartLanes::keyFits(inflight, f.len)) break;
        _port->write(f.b, f.len);
        inflight = (uint16_t)(inflight + f.len);
        ++_keyFrames;
        ++k;
    }
    return k;
}

void UartTx::retireHeld(uint8_t k)
{
    for (uint8_t i = 0; i < k && _kqN; ++i) {
        const KeyFrame& f = _kq[_kqHead];
        if (_tap) _tap('T', f.b, f.len, _ctx);
        _kqHead = (uint8_t)((_kqHead + 1) % KEY_HOLD);
        --_kqN;
    }
}

// Behind everything already held, so keys keep their order. A full hold (a
// backlog no real burst reaches) falls back to the old blocking write of the
// lot: late is better than a lost release.
bool UartTx::hold(const uint8_t* frame, uint8_t len)
{
    if (_kqN < KEY_HOLD && len <= KEY_FRAME_MAX) {
        if (_stampArmed) {
            uint16_t ahead = inflightNow();
            for (uint8_t i = 0; i < _kqN; ++i) ahead = (uint16_t)(ahead + _kq[(uint8_t)((_kqHead + i) % KEY_HOLD)].len);
            stampKey(ahead);
        }
        KeyFrame& f = _kq[(uint8_t)((_kqHead + _kqN) % KEY_HOLD)];
        f.len = len;
        memcpy(f.b, frame, len);
        ++_kqN;
        ++_keyHeld;
        return true;
    }
    if (xSemaphoreTake(_lock, LOCK_WAIT) != pdTRUE) {
        _keyDropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    uint16_t inflight = inflightNow();
    const uint8_t k = writeHeldLocked(inflight, true);
    stampKey(inflight);
    _port->write(frame, len);
    ++_keyFrames;
    xSemaphoreGive(_lock);
    retireHeld(k);
    if (_tap) _tap('T', frame, len, _ctx);
    return true;
}

// Counters are bumped inside the lock (two writer tasks). The tap runs outside
// it so a slow web client cannot delay a key press.
bool UartTx::writeKey(const uint8_t* frame, uint8_t len)
{
    if (!_port || !_lock) return false;
    if (xSemaphoreTake(_lock, LOCK_WAIT) == pdTRUE) {
        // Whatever is held goes first; this frame only if all of it went.
        uint16_t inflight = inflightNow();
        const uint8_t k = writeHeldLocked(inflight, false);
        bool wrote = false;
        if (k == _kqN && Sinks::UartLanes::keyFits(inflight, len)) {
            stampKey(inflight);
            _port->write(frame, len);
            ++_keyFrames;
            wrote = true;
        }
        xSemaphoreGive(_lock);
        retireHeld(k);
        if (wrote) {
            if (_tap) _tap('T', frame, len, _ctx);
            return true;
        }
    }
    // Over the budget, or the mutex was busy: the next pass sends it.
    return hold(frame, len);
}

void UartTx::pumpKeys()
{
    if (!_kqN || !_port || !_lock) return;
    if (xSemaphoreTake(_lock, LOCK_WAIT) != pdTRUE) return;
    uint16_t inflight = inflightNow();
    const uint8_t k = writeHeldLocked(inflight, false);
    xSemaphoreGive(_lock);
    retireHeld(k);
}

bool UartTx::writeAck(uint16_t* inflightOut)
{
    if (!_port || !_lock) return false;
    static const uint8_t ack = 0xFF;
    if (xSemaphoreTake(_lock, LOCK_WAIT) != pdTRUE) {
        _ackDropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (inflightOut) *inflightOut = inflightNow();
    _port->write(&ack, 1);
    ++_acks;
    xSemaphoreGive(_lock);
    return true;
}

UartTx::Wrote UartTx::writeLane(Lane lane, const uint8_t* frame, uint8_t len,
                                bool rxMidFrame, uint32_t msSinceRx, char tapDir)
{
    if (!_port || !_lock) return Wrote::Dropped;
    if (xSemaphoreTake(_lock, LOCK_WAIT) != pdTRUE) {
        _vehDropped.fetch_add(1, std::memory_order_relaxed);
        return Wrote::Dropped;
    }
    // Decided under the lock: a key written between the check and the copy
    // would otherwise have this frame land behind it, which is fine, but the
    // other way round is the delay the gate exists to prevent.
    if (!Sinks::UartLanes::mayWrite(lane, inflightNow(), rxMidFrame, len, msSinceRx)) {
        ++_deferred;
        xSemaphoreGive(_lock);
        return Wrote::Deferred;
    }
    _port->write(frame, len);
    if (lane == Lane::Reply) ++_replyFrames;
    else                     ++_vehFrames;
    xSemaphoreGive(_lock);
    if (_tap) _tap(tapDir, frame, len, _ctx);
    return Wrote::Written;
}
