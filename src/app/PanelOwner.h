#pragma once
#include "../Features.h"
#if FEATURE_DISPLAY_CAN
#include <Arduino.h>
#include <AffaDisplay.h>
#include "../app/UiWaits.h"
#include "../display/PanelOps.h"
#include "../display/PanelStatus.h"
#include "../display/TwaiAffaLink.h"
#include "../utils/Snapshot.h"

// The panel task: the Arduino loop task, and the owner of AffaDisplay's poll()
// (docs/design/event-tasks.md, Step 3, §3.6). begin() declares it the poll
// owner, which is also what lets every other task render: the library posts
// their calls into its own dispatch ring and admits them inside poll(), after
// the keys (src/display/PanelOps.h). poll() from any other task is a counted
// no-op (foreignPolls).
//
//   * pump() is one panel pass: bus-off recovery, then poll(). After it, the
//     status is published as a Snapshot<PanelStatus> for every other reader.
//   * waitTicks() says how the task may sleep: in twai_receive(), so a panel
//     key frame wakes it at once (TwaiBus::waitRx). One tick while the library
//     is busy (a render posted counts), else 2 ms (always 2 ms while the TX
//     gate is shut); the library's own deadlines are ~200 ms, so nothing it
//     times is late.
//   * serviceTx() is the TX gate (TxGate.h): no ACK and nothing heard for 2 s
//     shuts transmit and aborts the frame the controller keeps retrying, so a
//     missing panel is not 1400 bus errors a second on the key core.
//
// The library's callbacks (onSync, onComplete) arrive here, on this task, and
// since Step 4 they end here: the render policy is the ui task's. What it needs
// from them leaves in two ways:
//   * every ticket's verdict, as a Completion handed to Hooks::onCompletion,
//     which queues it for the ui task without waiting (the ui ignores the
//     tickets it is not waiting on);
//   * everything else as counts in PanelStatus (sync changes, the failure
//     streak), which the ui reads every pass, so a full queue can lose no edge.
// The delivery counters and the failure streak are kept here.
namespace App
{
    namespace Panel
    {
        struct Hooks {
            // Every ticket's verdict, on this task. Must not wait.
            void (*onCompletion)(const UiWaits::Completion& c) = nullptr;
        };

        struct Setup {
            affa::AffaDisplayBase*  display  = nullptr;
            affa::CarminatDisplay*  carminat = nullptr;   // same object, typed; or null
            MegaOpen::TwaiAffaLink* link     = nullptr;
            MegaOpen::PanelOps*     ops      = nullptr;   // its counts, for PanelStatus
            Hooks                   hooks;
        };

        // setup(), on the loop task, which becomes the owner. Wires the
        // library's callbacks and declares the poll owner. Call before the
        // library's begin(), or the first sync transition is missed.
        void begin(const Setup& s);

        // ---- the panel task only --------------------------------------------
        // setup()'s configuration and the pump. Renders go through PanelOps.
        affa::AffaDisplayBase& lib();

        // One panel pass. Keys leave inside poll(), straight onto the KeyBus.
        void pump(uint32_t nowMs);
        // The TX gate, on each new controller sample (every 100 ms).
        void serviceTx(uint32_t nowMs);
        // How long the next sleep may be: one tick while the library is busy,
        // else (or while gated) 2 ms. The loop sleeps in
        // TwaiBus::waitRx(), so a panel frame ends it early.
        TickType_t waitTicks();

        // The [panel] heartbeat line, and a new window for its maxima.
        void logWindow();

        // ---- any task --------------------------------------------------------
        // A panel object exists (fixed at boot). Never touches the library.
        bool present();
        // Published after every poll(). failStreak in it is the render
        // breaker's input; a Resync resets it.
        MegaOpen::PanelStatus status();
        const Snapshot<MegaOpen::PanelStatus>& statusSnapshot();
        // Inside poll() (the key callback): how long since the previous poll()
        // began, i.e. how long a key frame could have waited in the TWAI queue.
        uint32_t pollGapUs();
    }
}
#endif
