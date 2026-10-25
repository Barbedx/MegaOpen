#pragma once
#include "../Features.h"
#if FEATURE_CAN2
#include <Arduino.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "IVehicleSource.h"
#include "Autobaud.h"
#include "../utils/Snapshot.h"

// The car's bus on the T-2CAN: CAN A, an MCP2518FD on SPI (docs/design/event-
// tasks.md, Step 5). It replaces Mcp2518Bus:
//
// LISTEN-ONLY, IN THE CHIP. The controller's own ListenOnly mode never drives
// the bus — no ACK, no error frames, no TX — so a firmware bug cannot touch the
// car. The one exception is the diagnostic read (src/diag/ClimDiag.h, the
// owner's opt-in): setTransmit() puts the chip in normal mode, only on a
// locked or hand-set rate, and send() is its only way out. Any sign of trouble
// on the bus (TEC past the warning level, error passive, bus off) drops it
// straight back to listen-only and latches txFault().
//
// Classic CAN only: FD frames are counted and dropped (Frame carries 8 bytes),
// and so are remote frames, which carry no payload.
//
// OUR ISR, NOT THE LIBRARY'S. The library's isr() would wake its own
// "ACAN2517Handler" task (priority 16, any core), which drains the chip with
// interrupts masked and appends to the receive ring while another task removes
// from it, with no lock. Ours raises a flag and notifies the car task, so that
// task blocks for ever (1 kB, 0 wakes) and every SPI and ring access happens
// on the car task: begin(), the chip service, receive(), the error counters,
// the rate changes. Nothing else may call into this object except
// requestRate() and status().
//
// THE LEVEL IS THE TRUTH. INT is armed on a falling edge only after the chip
// has gone listen-only, and a frame in between leaves INT low with no edge to
// come. Every wake services the chip on the flag OR on INT simply
// being low; `intKicks` counts the times the level found what no edge said.
//
// THE RATE FINDS ITSELF (Autobaud.h). Listen-only on 500k, 250k, 125k, 1M in
// turn, the saved rate first, a hand-set rate only. Each change is end() and a
// fresh begin(): end() detaches the ISR, resets the chip, deletes the library's
// handler task and frees its rings; begin() creates them again. So nothing —
// task, ISR, buffer — outlives its rate (ACAN2517FD 2.1.16, ACAN2517FD.cpp:505,
// :509, :543-575).
class Mcp2518Source : public IVehicleSource {
public:
    static Mcp2518Source& instance();

    // setup(), before the car task starts: the hand-set rate (0 = auto) and
    // the last detected one (0 = none), both from NVS.
    void configure(uint32_t manualBps, uint32_t savedBps);
    // setup(): the task our ISR notifies (the car task). nullptr = flag only.
    void setNotifyTask(TaskHandle_t t);

    // ---- the car task --------------------------------------------------------
    const char* name() const override { return "can2"; }
    bool        begin() override;
    void        service(uint32_t nowMs) override;
    size_t      drain(Frame* out, size_t max, uint32_t nowMs) override;
    bool        notifies() const override { return true; }
    // The locked (or hand-set) rate; 0 while searching.
    uint32_t    bitrate() const override;
    Stats       stats() const override;

    // ---- any task --------------------------------------------------------------
    // httpd: a hand-set rate (0 = back to auto). The car task restarts the
    // search on its next wake. A single word, one writer.
    void requestRate(uint32_t manualBps);

    // ---- the diagnostic read (car task; src/diag/ClimDiag.h is the only user) --
    // Normal mode (the chip ACKs and may send) or back to listen-only: a
    // controller restart either way, a few ms of frames lost. false: refused,
    // because the rate is still being searched, or a fault is latched.
    bool setTransmit(bool on, uint32_t nowMs);
    bool transmitting() const { return _tx; }
    // One classic 11-bit frame. false: listen-only, or the chip's queue is full.
    bool send(uint32_t id, const uint8_t* data, uint8_t len);
    // Normal mode ended on its own (bus trouble); setTransmit(true) is refused
    // until clearTxFault().
    bool txFault() const { return _txFault; }
    void clearTxFault() { _txFault = false; }

    struct Status {
        bool     live = false;        // the controller is running, listen-only
        bool     tx = false;          // normal mode (the diagnostic read), not listen-only
        bool     txFault = false;     // normal mode ended on bus trouble
        uint32_t txFrames = 0;        // frames sent since boot
        uint8_t  state = 0;           // Autobaud::State: 0 searching, 1 locked, 2 manual
        uint32_t bps = 0;             // the rate it is listening on now
        uint32_t detected = 0;        // the rate that locked, 0 = none yet
        uint32_t fps = 0;             // frames in the last second
        uint32_t frames = 0, fdDropped = 0, remoteDropped = 0;
        uint32_t intKicks = 0;        // INT found low with no edge seen
        uint32_t maskMaxUs = 0;       // longest chip service, interrupts masked
        uint32_t hwOverflows = 0;     // chip FIFO overruns (saturates at 255)
        uint32_t ringPeak = 0;        // driver ring high-water, of 256
        uint32_t trec = 0;            // CiTREC: REC 7:0, TEC 15:8, flags 21:16
        uint32_t bdiag1 = 0;          // CiBDIAG1: error flags 31:16, error-free count 15:0
        uint32_t restarts = 0;        // controller begin()s, rate changes included
        uint32_t cycles = 0;          // fruitless rounds of the four rates
        uint32_t rejected = 0;        // rates given up for errors
        uint32_t beginErr = 0;        // the last begin()'s error bits, 0 = ok
    };
    // Published by the car task about 4 times a second; any task reads a copy.
    const Snapshot<Status>& status() const { return _snap; }

private:
    Mcp2518Source() = default;

    // Car task. end() if running, then begin() on `bps`: listen-only, or
    // normal mode while _tx.
    bool start(uint32_t bps, uint32_t nowMs);
    void stop();
    void publish(uint32_t nowMs);

    bool     _live = false;
    bool     _begun = false;          // begin() ran (succeeded or not) since the last end()
    bool     _spiUp = false;
    bool     _tx = false, _txFault = false;
    uint32_t _txFrames = 0, _txCheckAtMs = 0;
    uint32_t _manual = 0, _saved = 0;
    Bus::Autobaud _ab;
    std::atomic<uint32_t> _req{0xFFFFFFFFu};   // a requested manual rate, none = all ones

    // Counters: the car task's own.
    uint32_t _frames = 0, _fdDropped = 0, _remoteDropped = 0;
    uint32_t _intKicks = 0, _maskMaxUs = 0;
    uint32_t _rateFrames = 0;         // since the current rate started
    uint32_t _restarts = 0, _beginErr = 0;
    uint32_t _retryAtMs = 0;          // a failed begin() is retried from here
    uint32_t _errCheckAtMs = 0;
    uint32_t _fpsCount = 0, _fpsAtMs = 0, _fps = 0;
    uint32_t _publishAtMs = 0;
    uint32_t _quietLogAtMs = 0;       // the "still no bus" line, rate-limited
    uint32_t _trec = 0, _bdiag1 = 0;
    Snapshot<Status> _snap;
};
#endif
