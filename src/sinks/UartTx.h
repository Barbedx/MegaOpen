#pragma once
#include <Arduino.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "UartLanes.h"

// Sole owner of the head-unit UART (ARCHITECTURE.md §8). Two tasks write it,
// both on core 1 (docs/design/event-tasks.md §2.1, §3.5): the keys task (keys)
// and the hu task (acks, replies, vehicle and raw frames). One mutex keeps
// frames from interleaving; it is held only for a FIFO copy.
//
// Acks always go straight in; keys too, up to UartLanes::KEY_BUDGET bytes in
// the FIFO, and the rest of a burst waits here for pumpKeys(). Everything else
// goes in only when the FIFO is empty and the head unit is not mid-frame
// (UartLanes::mayWrite), so the most a key or an ack ever finds in front of it
// is one vehicle frame's worth of bytes.
class UartTx {
public:
    using TapFn = void (*)(char dir, const uint8_t* frame, uint8_t len, void* ctx);
    using Lane  = Sinks::UartLanes::Lane;

    void begin(HardwareSerial* port);
    void onTap(TapFn fn, void* ctx) { _tap = fn; _ctx = ctx; }

    // Into the FIFO now if it fits the key budget, else held (in order) for
    // pumpKeys(); a mutex not free within LOCK_WAIT also holds it rather than
    // losing it. False only when the frame is gone: no port yet, or the hold
    // was full and the mutex could not be had either (keyDropped).
    // The keys task only: the hold has no lock.
    bool writeKey(const uint8_t* frame, uint8_t len);

    // Held key frames into the FIFO, as far as the budget allows. The keys
    // task, every pass; while keysHeld(), it comes back after one tick.
    void pumpKeys();
    bool keysHeld() const { return _kqN != 0; }

    // Bare 0xFF. The head unit allows 10 ms. The hu task only. *inflightOut
    // gets the bytes that were already in the FIFO in front of it.
    bool writeAck(uint16_t* inflightOut = nullptr);

    // A reply (0x90) or a vehicle/raw frame, through the lane gate. The hu
    // task only: it knows whether the head unit is mid-frame and when it last
    // spoke. Deferred = the gate said not yet; the caller keeps the frame.
    enum class Wrote : uint8_t { Written, Deferred, Dropped };
    Wrote writeLane(Lane lane, const uint8_t* frame, uint8_t len,
                    bool rxMidFrame, uint32_t msSinceRx, char tapDir = 'V');

    // The key latency stamp. The keys task arms it before it handles a key;
    // the next writeKey records when its frame reached the FIFO and how many
    // bytes were already there. Keys-task-only fields: no lock.
    void armKeyStamp() { _stampArmed = true; _stampTaken = false; }
    bool takeKeyStamp(uint32_t& atUs, uint16_t& inflight)
    {
        _stampArmed = false;
        if (!_stampTaken) return false;
        _stampTaken = false;
        atUs = _stampAtUs;
        inflight = _stampInflight;
        return true;
    }

    uint32_t keyFrames() const { return _keyFrames; }
    uint32_t vehicleFrames() const { return _vehFrames; }
    uint32_t replyFrames() const { return _replyFrames; }
    // Lane frames the gate held back; the hu task retries them.
    uint32_t vehicleDeferred() const { return _deferred; }
    uint32_t acks() const { return _acks; }
    // Key frames the budget (or a busy mutex) held back for a later pass:
    // delayed, not lost.
    uint32_t keyHeld() const { return _keyHeld; }
    // Lock timeouts: the frame was dropped, not delayed.
    uint32_t keyDropped() const { return _keyDropped.load(std::memory_order_relaxed); }
    uint32_t ackDropped() const { return _ackDropped.load(std::memory_order_relaxed); }
    uint32_t vehDropped() const { return _vehDropped.load(std::memory_order_relaxed); }

private:
    static constexpr TickType_t LOCK_WAIT = pdMS_TO_TICKS(20);
    // The hold: a 12-event macro replay or a full batch of panel taps fits.
    // A key frame is 6 bytes; a longer one than KEY_FRAME_MAX is not held.
    static constexpr uint8_t KEY_HOLD      = 32;
    static constexpr uint8_t KEY_FRAME_MAX = 8;
    struct KeyFrame { uint8_t len; uint8_t b[KEY_FRAME_MAX]; };

    uint16_t inflightNow();
    // Under the lock: held frames into the FIFO, oldest first, while they
    // fit (all of them when `force`). Returns how many; retireHeld() then
    // taps and removes them, outside the lock.
    uint8_t  writeHeldLocked(uint16_t& inflight, bool force);
    void     retireHeld(uint8_t k);
    bool     hold(const uint8_t* frame, uint8_t len);
    void     stampKey(uint16_t ahead);

    HardwareSerial*   _port = nullptr;
    SemaphoreHandle_t _lock = nullptr;
    TapFn             _tap = nullptr;
    void*             _ctx = nullptr;
    // Bumped inside the lock: one writer at a time.
    uint32_t          _keyFrames = 0, _vehFrames = 0, _replyFrames = 0, _deferred = 0, _acks = 0;
    // The keys task's own: the hold and its counter.
    KeyFrame          _kq[KEY_HOLD] = {};
    uint8_t           _kqHead = 0, _kqN = 0;
    uint32_t          _keyHeld = 0;
    // Bumped exactly when the lock could NOT be had, so by either writer:
    // atomic, not "inside the lock".
    std::atomic<uint32_t> _keyDropped{0}, _ackDropped{0}, _vehDropped{0};

    bool     _stampArmed = false, _stampTaken = false;
    uint32_t _stampAtUs = 0;
    uint16_t _stampInflight = 0;
};
