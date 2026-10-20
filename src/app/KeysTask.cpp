#include "KeysTask.h"
#include "KeyBus.h"
#include "Rt.h"
#include "../sinks/UartTx.h"
#include "../sinks/UartLanes.h"
#include "../utils/LatencyHist.h"
#include "../utils/LogBus.h"

namespace App
{
    namespace KeysTask
    {
        namespace
        {
            constexpr uint32_t IDLE_WAIT_MS = 100;    // nothing pending: still beat
            constexpr uint8_t  BATCH        = 16;     // per pass, then deadlines run
            constexpr uint32_t LOG_MS       = 10000;  // the [key] line

            Hooks            s_h;
            UartTx*          s_tx = nullptr;
            Keys::KeyRouter  s_router;
            int              s_id = -1;

            // srcToWrite: from the key's source time to its frame entering the
            // FIFO. wire: plus the bytes it found there, at 260 us each.
            LatencyHist s_src, s_wire;
            uint32_t    s_pollGapMax = 0;

            // The [key] line: this window's histogram, and the bus counters
            // since boot. Logged by the owner, so the histogram has one writer
            // and is reset without a race.
            void logWindow()
            {
                // tapBy = the tap-lane drops by origin (srcId 0 matrix, 1 matrix
                // encoder, 3 panel, 4 knobs): which producer outran the lane.
                LogBus::printf("[key] n=%u p50=%uus p99=%uus max=%uus wireMax=%uus pollGapMax=%uus "
                               "drops(tap/rel/web)=%u/%u/%u tapBy(mtx/enc/panel/knob)=%u/%u/%u/%u "
                               "stuckSynth=%u held=%u",
                               (unsigned)s_src.count(), (unsigned)s_src.p50(), (unsigned)s_src.p99(),
                               (unsigned)s_src.max(), (unsigned)s_wire.max(), (unsigned)s_pollGapMax,
                               (unsigned)KeyBus::drops(Keys::Lane::Tap),
                               (unsigned)KeyBus::drops(Keys::Lane::Rel),
                               (unsigned)KeyBus::drops(Keys::Lane::Web),
                               (unsigned)KeyBus::tapDropsFrom(0), (unsigned)KeyBus::tapDropsFrom(1),
                               (unsigned)KeyBus::tapDropsFrom(3), (unsigned)KeyBus::tapDropsFrom(4),
                               (unsigned)s_router.stuckSynth(),
                               // held = key frames the FIFO budget sent a pass later
                               (unsigned)(s_tx ? s_tx->keyHeld() : 0));
                s_src.reset();
                s_wire.reset();
                s_pollGapMax = 0;
            }

            void handle(const Keys::KeyMsg& m)
            {
                if (m.kind == Keys::KeyMsg::Kind::PanelKey && m.aux > s_pollGapMax)
                    s_pollGapMax = m.aux;

                if (s_tx) s_tx->armKeyStamp();
                s_router.handle(m);

                uint32_t atUs = 0;
                uint16_t inflight = 0;
                // Web keys are tests, not the latency the car feels.
                if (s_tx && s_tx->takeKeyStamp(atUs, inflight) && m.ev.srcId != 2) {
                    const uint32_t src = atUs - m.atUs;
                    s_src.record(src);
                    s_wire.record(src + Sinks::UartLanes::wireUs(inflight));
                }
            }

            void run(void*)
            {
                uint32_t logAt = millis();
                for (;;) {
                    // Sleep until the next deadline exactly, a post, or 100 ms.
                    uint32_t waitMs = IDLE_WAIT_MS;
                    const uint32_t before = millis();
                    uint32_t due = 0;
                    if (s_h.nextDue && s_h.nextDue(before, due)) {
                        const int32_t left = (int32_t)(due - before);
                        waitMs = left <= 0 ? 0 : (uint32_t)left;
                        if (waitMs > IDLE_WAIT_MS) waitMs = IDLE_WAIT_MS;
                    }
                    // At least a tick, even for a deadline already due: a sink
                    // whose deadline never clears must cost a late release, not
                    // a spin at priority 7 that starves the panel. A pending post
                    // still returns at once (the notification is already set).
                    // Key frames held back by the FIFO budget: one tick, which
                    // is less than the FIFO's KEY_BUDGET takes to drain.
                    if (s_tx && s_tx->keysHeld()) waitMs = 1;
                    if (waitMs < 1) waitMs = 1;
                    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs));

                    const uint32_t t0 = micros();
                    // Held frames first: they are older than anything below.
                    if (s_tx) s_tx->pumpKeys();
                    if (s_h.apply) s_h.apply();

                    Keys::KeyMsg m;
                    uint8_t n = 0;
                    while (n < BATCH && KeyBus::pop(m)) { handle(m); ++n; }
                    if (n == BATCH) {
                        // More waiting: come straight back after the deadlines.
                        xTaskNotifyGive(xTaskGetCurrentTaskHandle());
                    } else {
                        // Only the lost releases whose earlier presses have all
                        // been handled (no older item left in a key lane), so
                        // continuous traffic queued after them cannot hold them
                        // back. `all`: more were lost than could be named, so
                        // every key still down is let go.
                        Keys::Event lost[Keys::KeyQueues::LOST_MAX];
                        bool all = false;
                        const uint8_t k = KeyBus::takeLostReady(lost, Keys::KeyQueues::LOST_MAX, all);
                        if (k)   s_router.synthesise(lost, k, millis());
                        if (all) s_router.releaseAll(millis());
                    }

                    const uint32_t now = millis();
                    if (s_h.deadlines) s_h.deadlines(now);

                    if ((uint32_t)(now - logAt) >= LOG_MS) {
                        logAt = now;
                        logWindow();
                    }
                    Tasks::beat(s_id, micros() - t0);
                }
            }
        }

        void observe(const Keys::Event& e) { s_router.observe(e); }

        void begin(const Hooks& h, UartTx* tx)
        {
            s_h  = h;
            s_tx = tx;
            s_router.begin(h.route);

            // Critical: dead keys are the one fault the OTA trial must catch.
            TaskSpec spec;
            spec.name     = "keys";
            spec.fn       = run;
            spec.stack    = Plan::KEYS_STACK;
            spec.prio     = Plan::KEYS_PRIO;
            spec.core     = Plan::KEYS_CORE;
            spec.critical = true;
            spec.idOut    = &s_id;
            const TaskHandle_t t = spawn(spec);
            KeyBus::setConsumer(t);
            // Anything posted before the handle was known gets handled now.
            if (t) xTaskNotifyGive(t);
        }
    }
}
