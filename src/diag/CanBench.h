#pragma once
#include "../Features.h"
#if FEATURE_DISPLAY_CAN || FEATURE_VEHICLE_CAN
#include <Arduino.h>
#include <driver/twai.h>
#include "../bus/CanRole.h"
#include "../bus/ICanBus.h"
#include "../bus/TwaiBus.h"

// The CAN controller's loopback test and its status, run by the one task
// that owns the TWAI driver: the loop (the panel task in the display role)
// (docs/design/event-tasks.md, Step 3).
//
// The loopback test takes the pins off the driver, so it must not run while
// the loop sits inside twai_receive() or twai_transmit() on the other core. It
// is a Request/Reply op: httpd posts it and waits up to 2 s, the loop runs it
// between two poll()s, and the answer comes back in the slot. Nothing else
// calls twai_* in this firmware except through this module and TwaiBus on that
// task.
//
// The controller's counters are sampled here every 100 ms and published as a
// snapshot, so /api/can and the heartbeat read a copy. The
// alerts are read here too: twai_read_alerts() consumes them, so one reader
// is the only honest one.
namespace Diag
{
    // One sample of the controller, for readers on any task.
    struct CanStatus {
        bool     installed = false;
        uint8_t  state     = 0;       // twai_state_t
        uint8_t  mode      = 0;       // twai_mode_t the driver runs in
        uint32_t tec = 0, rec = 0;
        uint32_t txFailed = 0, rxMissed = 0, busErr = 0, arbLost = 0;
        uint32_t msgsToTx = 0, msgsToRx = 0;
        // Alerts raised in the last one to two seconds.
        uint32_t alerts = 0;
        // Our own tallies (TwaiBus): did we ever call transmit, and did it take.
        uint32_t twaiTxOk = 0, twaiTxFail = 0, twaiTxBusy = 0, twaiRx = 0;
        uint32_t installs = 0;
        uint32_t atMs = 0;            // when it was sampled
    };

    namespace CanBench
    {
        struct Setup {
            Bus::Role          role = Bus::Role::Display;
            // What a reinstall wires back up (twaiRestartInRole in main.cpp
            // before this moved): the vehicle role's receive handler and the
            // transmit tap that feeds the raw frame ring.
            RxHandler          rx    = nullptr;
            TwaiBus::TxTap     txTap = nullptr;
            bool               listenOnly = true;
        };

        // setup(), on the task that will own the driver, before httpd starts.
        void begin(const Setup& s);

        // The owner, once a pass: queued web ops, then the 100 ms sample.
        void service(uint32_t nowMs);

        // ---- the owner's own reads (same task, no copy) ------------------------
        // The newest raw sample; seq() counts them, so the TX gate runs once per
        // sample and not once per pass.
        const twai_status_info_t& info();
        bool                      infoValid();
        uint32_t                  seq();

        // ---- any task ------------------------------------------------------------
        CanStatus status();
        // Whether the node is meant to stay off the bus (one writer: the owner).
        bool listenOnly();

        // ---- httpd: each runs on the owner; false = no answer within 2 s --------
        bool loopTest(uint32_t& cycles, uint32_t& followed);
    }
}
#endif
