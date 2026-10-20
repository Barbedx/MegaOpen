#pragma once
#include "../Features.h"
#if FEATURE_DISPLAY_CAN || FEATURE_VEHICLE_CAN
#include <Arduino.h>
#include <driver/twai.h>
#include "../bus/ICanBus.h"
#include "../bus/TwaiBus.h"

// The CAN controller's status, sampled by the one task that owns the TWAI
// driver: the loop (the panel task when there is a panel)
// (docs/design/event-tasks.md, Step 3). Nothing else calls twai_* in this
// firmware except through this module and TwaiBus on that task.
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
            bool               listenOnly = true;
        };

        // setup(), on the task that will own the driver, before httpd starts.
        void begin(const Setup& s);

        // The owner, once a pass: the 100 ms sample.
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
    }
}
#endif
