#pragma once
#include "../Features.h"
#if FEATURE_DIAG
#include <stdint.h>
#include "../bus/Frame.h"

namespace Vehicle { class State; }

// The climate ECU's `21 C1`, read on CAN A (src/diag/ClimDiag.h): the cabin
// temperature and the sun sensor, which no broadcast frame carries.
//
// OFF UNLESS THE OWNER SWITCHES IT ON (NVS "diag": on, interval). Two ways in:
//   * a probe: one request now, its reply in the log ([diag]), then back to
//     listen-only;
//   * polling: one request every `interval` seconds while the car's bus is
//     alive; the controller stays in normal mode while it runs.
// Never while the rate is being searched, never on a quiet bus (no frame for
// BUS_QUIET_MS: the car is off, and nobody would ACK), never after a bus fault
// (Mcp2518Source::txFault(), cleared by switching the read on again). A
// negative reply stops the polling: the ECU says no, and asking again changes
// nothing. Three timeouts in a row back off for BACKOFF_MS.
//
// Everything that touches the bus runs on the car task (frame(), loop()); the
// switches are atomics any task may set.
namespace App
{
    namespace CarDiag
    {
        constexpr uint32_t BUS_QUIET_MS = 2000;
        constexpr uint32_t BACKOFF_MS   = 60000;
        constexpr uint8_t  INTERVAL_MIN_S = 2, INTERVAL_MAX_S = 60, INTERVAL_DEFAULT_S = 5;

        // setup(): the switches from NVS. Before the car task starts.
        void begin(Vehicle::State* state);

        // ---- the car task ---------------------------------------------------------
        void frame(const Frame& f, uint32_t nowMs);
        void loop(uint32_t nowMs);

        // ---- any task ---------------------------------------------------------------
        // Saved to NVS; switching on also clears a latched bus fault and a stop.
        // nullptr or a refusal code.
        const char* set(bool on, uint8_t intervalS);
        void probe();
        // The owner's one-shot check of every variant, each step logged with
        // its reply and the bus around it ([diag] sweep): 21 C1 without a
        // session; 10 C0; 21 C1 in it; 3E 01; 21 C1; 10 81 (always, last).
        // The bus line (frames and distinct ids a second) shows a cluster
        // that reboots: its ids go missing. Polling waits meanwhile.
        void sweep();

        struct View {
            bool     on = false;
            uint8_t  intervalS = INTERVAL_DEFAULT_S;
            const char* state = "off";    // off | idle | waiting | quiet | backoff | stopped | fault | refused | sweep
            const char* last = "";        // Exchange::statusId of the last result
            uint8_t  nrc = 0;
            uint8_t  reply[24] = {};      // the last reply's first bytes
            uint8_t  replyLen = 0;
            uint32_t lastAgoMs = 0;       // 0: none
            uint32_t ok = 0, timeouts = 0, negatives = 0, bad = 0, sent = 0;
            bool     tx = false;          // the controller is in normal mode
            int16_t  cabinX10 = 0;        // the last good values
            uint16_t sunWm2 = 0;
            bool     haveValues = false;
        };
        void view(View& out);
    }
}
#endif
