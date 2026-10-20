#pragma once
#include "../Features.h"
#if FEATURE_DISPLAY_CAN
#include <stdint.h>

// What the panel task knows about the panel, as one small copy (docs/design/
// event-tasks.md, Step 3). Any task may render, but the library's observers
// are one consistent moment only on the task that calls poll(); this is what
// everyone else reads instead. The panel task publishes it after every poll()
// through a Snapshot<PanelStatus> (App::Panel::status()): httpd's /api/display,
// /api/health and /api/media, and since Step 4 the ui task's render policy,
// all read the copy.
//
// Plain bytes, no library types, so the struct stays a POD that can be copied
// under a spinlock. The enums are the library's own values (affa::SyncState,
// affa::Phase, affa::LossReason, affa::Result, affa::RenderSlot), cast back by
// the reader.
namespace MegaOpen
{
    // The panel's capabilities, one bit each, as supports(affa::Feature) said
    // at begin: they are fixed by the panel type.
    enum PanelFeature : uint8_t {
        PF_TEXT = 1u << 0, PF_TIME = 1u << 1, PF_POWER = 1u << 2, PF_MENU = 1u << 3,
        PF_POPUP = 1u << 4, PF_FULLSCREEN = 1u << 5, PF_CONFIRM = 1u << 6, PF_INFO = 1u << 7,
    };

    struct PanelStatus {
        bool     attached   = false;    // a panel object exists in this image
        bool     synced     = false;
        bool     registered = false;
        bool     busy       = false;    // a job in flight or queued
        bool     passive    = false;
        uint8_t  syncState  = 0;        // affa::SyncState bits
        uint8_t  phase      = 0;        // affa::Phase
        uint8_t  queued     = 0;        // jobs behind the active one
        uint8_t  lastResult = 0;        // affa::Result
        uint8_t  lastRendered = 0;      // affa::RenderSlot
        uint8_t  lastLoss   = 0;        // affa::LossReason
        uint8_t  features   = 0;        // PanelFeature bits
        uint32_t lastRenderedMs = 0;
        uint32_t sessionsLost   = 0;

        // The link recovery counters (affa::AffaDisplayBase::LinkHealth).
        uint32_t linkFlaps = 0, linkRecoveries = 0, linkFailures = 0, linkDownMs = 0;

        // The panel's own answers (onComplete), not enqueue acceptance: these
        // are what stop climbing when the glass freezes.
        uint32_t delivered = 0, superseded = 0, failed = 0;
        uint8_t  lastDelivery = 0;      // affa::Result
        // Consecutive delivery failures: the render breaker's input.
        uint16_t failStreak = 0;

        // The pump. foreignPolls should stay 0; pollLateMaxUs is the longest
        // gap between two poll() starts in this 10 s window.
        uint32_t polls = 0;
        uint32_t foreignPolls = 0;
        uint32_t pollLateMaxUs = 0;
        // Renders accepted (PanelOps, every task), and refused because the
        // library's dispatch ring was full.
        uint32_t queuePosted = 0, queueDropped = 0;

        // The link's TX gate: shut while nobody ACKs us (panel unplugged).
        bool     txGated = false;
        uint32_t txGates = 0;           // times it shut since boot

        // ---- for the ui task (Step 4), which no longer sees the callbacks ----
        // A Carminat panel: the OEM fullscreen geometry and the 0x1F1 pane.
        bool     carminat = false;
        // Sync state changes (both edges), counted, so a reader that looks
        // every 50 ms misses no edge.
        uint32_t syncChanges = 0;
        // Automatic repaints refused because a person's screen overtook them.
        uint32_t yielded = 0;
    };
}
#endif
