#pragma once
#include "../Features.h"
#if FEATURE_BLE
#include "BleKeyProxy.h"
#include "AmsVolumeStep.h"

// Keys -> the phone (Apple Media Service) and keys -> the head unit as a BLE
// keyboard (HID consumer control): the half that runs on ble_svc.
//
// AMS reaches whatever is playing; HID reaches the device driving the amplifier.
// Proven split: transport to AMS, volume to HID, hence a class mask per proxy
// (BleKeyProxy.h, on the keys task). What arrives here is already routed.
//
// One AMS write per pass (docs/design/event-tasks.md, Step 2). A write with
// response blocks this task for 30-130 ms, and ble_svc also has ServiceAms and
// the advertising to run, so the commands are paced, not burst:
//   * HID presses go first and at once (a notify, no response to wait for);
//   * other AMS commands (play, next, previous) go in arrival order;
//   * volume presses only add to a signed step counter — as many AMS steps a
//     press as make one iPhone button step, measured from the volume the
//     phone reports (AmsVolumeStep.h; it was a fixed fifteen) — and one step
//     goes per pass after those, so VOL+ then VOL- quickly is a net zero;
//   * anything that waited more than 300 ms is dropped and counted: a track
//     skip a second late is worse than none.
namespace Sinks
{
    class BleKeyWorker {
    public:
        static constexpr uint32_t STALE_MS     = 300;
        static constexpr uint8_t  VOL_PRESSES_MAX = 3;  // pending presses' worth, either way
        // A burst's last report may trail its last write by a connection
        // event or two; it is measured this long after it ended.
        static constexpr uint32_t VOL_SETTLE_MS = 400;
        static constexpr uint8_t  FIFO_MAX     = 4;
        // HidRole.cpp's KEY_HOLD_MS: Hid::service() releases this long after
        // the press.
        static constexpr uint32_t HID_HOLD_MS  = 30;

        // ble_svc, once per pass. Waits up to waitMs for the first command,
        // takes whatever else is queued, and presses the HID keys among them.
        void collect(uint32_t waitMs);
        // Then at most one AMS write. True while AMS work is still pending.
        bool step();
        bool pending() const { return _fifoN || _vol; }

        // A HID press went out less than HID_HOLD_MS ago, so its release is
        // still owed; `leftMs` = until it is due. An AMS write holds this task
        // for 30-130 ms, so ble_svc skips step() while this is true and the
        // release goes out on time, not behind the write.
        bool hidHoldLeft(uint32_t nowMs, uint32_t& leftMs) const;

        uint32_t amsWrites() const   { return _amsWrites; }   // that succeeded
        uint32_t amsFailed() const   { return _amsFailed; }
        uint32_t hidPresses() const  { return _hidPresses; }
        uint32_t stale() const       { return _stale; }
        uint32_t coalesced() const   { return _coalesced; }
        uint32_t noLink() const      { return _noLink; }
        uint8_t  volSteps() const    { return _volStep.steps(); }

    private:
        void accept(const BleCmd& c, uint32_t nowMs);
        // The current volume burst: measured and logged once it has settled.
        void volBurstBegin(int8_t dir, uint32_t nowMs);
        void volBurstSettle(uint32_t nowMs);

        BleCmd   _fifo[FIFO_MAX];
        uint8_t  _fifoN = 0;
        int16_t  _vol = 0;            // signed AMS volume steps still to send
        AmsVolumeStep _volStep;
        struct Burst {
            int8_t   dir = 0;         // 0 = none
            uint16_t writes = 0;
            float    v0 = 0;
            uint32_t reports0 = 0;
            uint32_t startMs = 0, endMs = 0;
            bool     clean = true;    // false: began while the one before was unmeasured
        } _burst;
        bool     _hidHeld = false;
        uint32_t _hidPressAtMs = 0;
        uint32_t _amsWrites = 0, _hidPresses = 0, _stale = 0, _coalesced = 0, _noLink = 0;
        uint32_t _amsFailed = 0;      // writes the stack refused (not retried)
        uint32_t _lastWriteMs = 0;    // paces writes without response
    };
}
#endif
