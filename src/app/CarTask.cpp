#include "../Features.h"
#include "CarTask.h"
#include "Rt.h"
#include "../vehicle/CanDecode.h"
#include "../utils/LogBus.h"
#include "../utils/Snapshot.h"
#include "freertos/queue.h"
#include <esp_heap_caps.h>
#include <new>
#include <atomic>
#if FEATURE_CAPTURE
#include "../capture/CanRecorder.h"
#endif

namespace App
{
    namespace Car
    {
        namespace
        {
            constexpr uint8_t  CMD_DEPTH  = 16;
            constexpr uint32_t PUBLISH_MS = 250;
            constexpr uint32_t LOG_MS     = 10000;   // the [car] line

            enum class Op : uint8_t { Pin, Unpin, Test };
            struct Cmd {
                Op       op = Op::Pin;
                uint8_t  sig = 0;
                int32_t  value = 0;
                uint32_t ms = 0;        // Test: how long
            };

            // The tests running now: a few at once is plenty for one person
            // pressing a list of test buttons. The soonest to end is reused
            // when all are taken.
            constexpr uint8_t MAX_TESTS = 8;
            constexpr uint8_t NO_SIG    = 0xFF;
            struct Tests {
                uint8_t  sig[MAX_TESTS];
                uint32_t untilMs[MAX_TESTS];
                Tests() { for (uint8_t i = 0; i < MAX_TESTS; ++i) { sig[i] = NO_SIG; untilMs[i] = 0; } }
            };

            Setup              s_s;
            Vehicle::CanDecode s_decode;
            StaticQueue_t      s_cmdBuf;
            uint8_t            s_cmdStore[CMD_DEPTH * sizeof(Cmd)];
            QueueHandle_t      s_cmd = nullptr;
            TaskHandle_t       s_task = nullptr;
            int                s_id = -1;
            std::atomic<uint32_t> s_cmdDrops{0};
            Snapshot<Stats>    s_stats;
            // Two ~1.3 kB buffers, placed in begin(): PSRAM where there is some.
            Staged<Vehicle::DecodeTable, true> s_table;
            // The owner's: the tests running, and the constants it pinned.
            Tests              s_tests;
            Snapshot<Tests>    s_testsPub;
            uint8_t            s_constSig[Vehicle::DecodeTable::MAX_CONSTS];
            uint8_t            s_nConst = 0;

            // The task's own: one wake's frames, and this window's evidence.
            // Static, not on the stack: 64 frames are 1.3 kB.
            Frame    s_buf[DRAIN_MAX];
            uint32_t s_frames = 0, s_fpsCount = 0, s_fps = 0;
            uint32_t s_drainMax = 0, s_busyMaxUs = 0;
            uint32_t s_rate[MAX_SOURCES] = {};

            bool testing(uint8_t sig)
            {
                for (uint8_t i = 0; i < MAX_TESTS; ++i) if (s_tests.sig[i] == sig) return true;
                return false;
            }

            void endTest(uint8_t sig)
            {
                for (uint8_t i = 0; i < MAX_TESTS; ++i) if (s_tests.sig[i] == sig) s_tests.sig[i] = NO_SIG;
            }

            // The signal goes back to its binding: its constant, else CAN.
            void giveBack(uint8_t sig)
            {
                const Vehicle::DecodeTable& t = s_table.live();
                const int k = t.constOf(sig);
                if (k >= 0) s_s.state->pin(sig, t.k[k].value);
                else        s_s.state->unpin(sig);
            }

            void pinConsts()
            {
                const Vehicle::DecodeTable& t = s_table.live();
                for (uint8_t i = 0; i < t.nK; ++i)
                    if (!testing(t.k[i].sig)) s_s.state->pin(t.k[i].sig, t.k[i].value);
            }

            void applyCmd(const Cmd& c, uint32_t now)
            {
                Vehicle::State& st = *s_s.state;
                switch (c.op) {
                    case Op::Pin:
                        endTest(c.sig);
                        st.pin(c.sig, c.value);
                        break;
                    case Op::Unpin:
                        endTest(c.sig);
                        giveBack(c.sig);
                        break;
                    case Op::Test: {
                        uint8_t slot = 0;
                        for (uint8_t i = 0; i < MAX_TESTS; ++i) {
                            if (s_tests.sig[i] == c.sig) { slot = i; break; }
                            if (s_tests.sig[slot] == NO_SIG) continue;
                            if (s_tests.sig[i] == NO_SIG ||
                                (int32_t)(s_tests.untilMs[i] - s_tests.untilMs[slot]) < 0) slot = i;
                        }
                        // All taken: the one that ends soonest ends now.
                        if (s_tests.sig[slot] != NO_SIG && s_tests.sig[slot] != c.sig) {
                            const uint8_t old = s_tests.sig[slot];
                            s_tests.sig[slot] = NO_SIG;
                            giveBack(old);
                        }
                        s_tests.sig[slot] = c.sig;
                        s_tests.untilMs[slot] = now + c.ms;
                        st.pin(c.sig, c.value);
                        break;
                    }
                }
                s_testsPub.publish(s_tests);
            }

            void expireTests(uint32_t now)
            {
                bool changed = false;
                for (uint8_t i = 0; i < MAX_TESTS; ++i) {
                    if (s_tests.sig[i] == NO_SIG || (int32_t)(s_tests.untilMs[i] - now) > 0) continue;
                    const uint8_t sig = s_tests.sig[i];
                    s_tests.sig[i] = NO_SIG;
                    giveBack(sig);
                    changed = true;
                }
                if (changed) s_testsPub.publish(s_tests);
            }

            // A new table is live: CanDecode reads it in place, the constants
            // that went are unpinned and the new ones pinned.
            void applyTable()
            {
                const Vehicle::DecodeTable& t = s_table.live();
                if (!s_decode.load(t.row, t.nRows)) {
                    // Never keep the old pointer: that buffer is the staging one now.
                    s_decode.load(nullptr, 0);
                    LogBus::line("[car] decode map REFUSED by CanDecode: no signals decoded");
                }
                if (!s_s.state) return;
                for (uint8_t i = 0; i < s_nConst; ++i)
                    if (t.constOf(s_constSig[i]) < 0 && !testing(s_constSig[i]))
                        s_s.state->unpin(s_constSig[i]);
                s_nConst = t.nK;
                for (uint8_t i = 0; i < t.nK; ++i) s_constSig[i] = t.k[i].sig;
                pinConsts();
            }

            // The owner's chores between frames: commands, a staged table,
            // tests running out.
            void chores(uint32_t now)
            {
                if (!s_s.state) return;
                Cmd c;
                while (s_cmd && xQueueReceive(s_cmd, &c, 0) == pdTRUE) applyCmd(c, now);
                if (s_table.applyIfDirty()) applyTable();
                expireTests(now);
            }

            void post(const Cmd& c)
            {
                if (!s_s.state) return;
                // No task: the loop drains the queue (service()), so State
                // keeps one writer.
                if (!s_cmd || xQueueSend(s_cmd, &c, 0) != pdTRUE) {
                    s_cmdDrops.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (s_task) xTaskNotifyGive(s_task);
            }

            Vehicle::DecodeTable* newTable(bool psram)
            {
                void* m = psram ? heap_caps_malloc(sizeof(Vehicle::DecodeTable), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                : nullptr;
                if (!m) m = heap_caps_malloc(sizeof(Vehicle::DecodeTable), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                configASSERT(m);   // setup, ~1.3 kB: there is no running without it
                return new (m) Vehicle::DecodeTable();
            }

            // The NVS blob, else the factory table. Setup only.
            void loadTable(const uint8_t* blob, size_t n)
            {
                const bool ps = psramFound();
                s_table.place(newTable(ps), newTable(ps));
                bool fromNvs = false;
                s_table.edit([&](Vehicle::DecodeTable& t) {
                    fromNvs = n && t.unpack(blob, n);
                    if (!fromNvs) t.factory();
                });
                s_table.applyIfDirty();
                applyTable();
                const Vehicle::DecodeTable& t = s_table.live();
                LogBus::printf("[car] decode map: %s, %u rows, %u constants, in %s", fromNvs ? "NVS" :
                               n ? "factory (the NVS blob did NOT unpack)" : "factory",
                               (unsigned)t.nRows, (unsigned)t.nK, ps ? "PSRAM" : "DRAM");
            }

            void frame(const Frame& f, uint32_t now)
            {
                if (s_s.state) s_decode.apply(f, *s_s.state, now);
                if (s_s.tap) s_s.tap(f);
                if (s_s.sniffer) {
                    // 11-bit and 29-bit ids apart: bit 31 marks
                    // an extended one in the table.
                    const uint32_t id = f.extended ? (f.id | 0x80000000u) : f.id;
                    s_s.sniffer->ingest(id, f.data, f.len, now);
                }
#if FEATURE_CAPTURE
                Capture::push(f, Capture::CAR, millis());
#endif
            }

            void publish(uint32_t)
            {
                Stats s;
                s.frames        = s_frames;
                s.fps           = s_fps;
                s.drainMax      = s_drainMax;
                s.decodeHits    = s_decode.hits();
                s.decodeUnknown = s_decode.unknown();
                s.decodeShort   = s_decode.short_();
                s.cmdDrops      = s_cmdDrops.load(std::memory_order_relaxed);
                s.busyMaxUs     = s_busyMaxUs;
                for (uint8_t i = 0; i < s_s.n; ++i) s.srcDropped += s_s.src[i]->stats().dropped;
                s.sources       = s_s.n;
                s_stats.publish(s);
            }

            void logWindow()
            {
                // fps = frames in the last second; drainMax = most frames one
                // wake took from one source (64 = it had to come back); decode
                // = frames matched/unmatched by the table; busyMax = the
                // longest wake. Then each source: frames/dropped.
                char src[96];
                int n = 0;
                src[0] = '\0';
                for (uint8_t i = 0; i < s_s.n && n < (int)sizeof(src) - 1; ++i) {
                    const IVehicleSource::Stats st = s_s.src[i]->stats();
                    n += snprintf(src + n, sizeof(src) - n, "%s%s=%lu/%lu", i ? " " : "",
                                  s_s.src[i]->name(), (unsigned long)st.frames,
                                  (unsigned long)st.dropped);
                }
                LogBus::printf("[car] fps=%lu frames=%lu drainMax=%lu decode=%lu/%lu short=%lu "
                               "busyMax=%luus cmdDrops=%lu src %s",
                               (unsigned long)s_fps, (unsigned long)s_frames,
                               (unsigned long)s_drainMax, (unsigned long)s_decode.hits(),
                               (unsigned long)s_decode.unknown(), (unsigned long)s_decode.short_(),
                               (unsigned long)s_busyMaxUs,
                               (unsigned long)s_cmdDrops.load(std::memory_order_relaxed), src);
                s_drainMax = 0;
                s_busyMaxUs = 0;
            }

            void run(void*)
            {
                // The sources' hardware becomes this task's here.
                bool polled = false;
                for (uint8_t i = 0; i < s_s.n; ++i) {
                    s_s.src[i]->begin();
                    if (!s_s.src[i]->notifies()) polled = true;
                }
                const TickType_t idle = pdMS_TO_TICKS(polled ? 2 : 20);
                uint32_t publishAt = millis(), logAt = publishAt, fpsAt = publishAt;
                bool more = false;

                for (;;) {
                    // A source that filled its 64 comes straight back, after
                    // anything else at this priority has had a turn.
                    if (more) taskYIELD();
                    else      ulTaskNotifyTake(pdTRUE, idle);

                    const uint32_t t0 = micros();
                    const uint32_t now = millis();

                    chores(now);
                    // A web reset/baseline lands here when no frame arrives.
                    if (s_s.sniffer) s_s.sniffer->applyRequests();

                    more = false;
                    for (uint8_t i = 0; i < s_s.n; ++i) {
                        IVehicleSource* src = s_s.src[i];
                        src->service(now);
                        const size_t got = src->drain(s_buf, DRAIN_MAX, now);
                        for (size_t k = 0; k < got; ++k) frame(s_buf[k], now);
                        s_frames += (uint32_t)got;
                        s_fpsCount += (uint32_t)got;
                        if (got > s_drainMax) s_drainMax = (uint32_t)got;
                        if (got >= DRAIN_MAX) more = true;

                        // A rate found (or set): the recorder's file header says it.
                        const uint32_t bps = src->bitrate();
                        if (bps && bps != s_rate[i]) {
                            s_rate[i] = bps;
#if FEATURE_CAPTURE
                            Capture::setBitrate(Capture::CAR, bps);
#endif
                        }
                    }

                    if ((uint32_t)(now - fpsAt) >= 1000) {
                        fpsAt = now;
                        s_fps = s_fpsCount;
                        s_fpsCount = 0;
                    }
                    const uint32_t busy = micros() - t0;
                    if (busy > s_busyMaxUs) s_busyMaxUs = busy;
                    if ((uint32_t)(now - publishAt) >= PUBLISH_MS) {
                        publishAt = now;
                        publish(now);
                    }
                    if ((uint32_t)(now - logAt) >= LOG_MS) {
                        logAt = now;
                        logWindow();
                    }
                    Tasks::beat(s_id, busy);
                }
            }
        }

        void begin(const Setup& s)
        {
            s_s = s;
            if (s_s.n > MAX_SOURCES) s_s.n = MAX_SOURCES;
            s_cmd = xQueueCreateStatic(CMD_DEPTH, sizeof(Cmd), s_cmdStore, &s_cmdBuf);
            loadTable(s.decodeBlob, s.decodeLen);
            if (s_s.sniffer) {
                // Off where the capture recorder is the tool (T-2CAN): a busy car
                // bus then costs no table search per frame. /api/can/sniff.
                s_s.sniffer->setEnabled(!FEATURE_CAN2);
            }
        }

        void start()
        {
            if (!s_s.n) return;
            // Critical: a hung car task is a frozen State, sent on to the head
            // unit as if it were current.
            TaskSpec spec;
            spec.name     = "car";
            spec.fn       = run;
            spec.stack    = Plan::CAR_STACK;
            spec.prio     = Plan::CAR_PRIO;
            spec.core     = Plan::CAR_CORE;
            spec.critical = true;
            spec.idOut    = &s_id;
            s_task = spawn(spec);
            LogBus::printf("[car] task: %u source(s), %u decode rows", (unsigned)s_s.n,
                           (unsigned)s_decode.rows());
        }

        bool         running() { return s_task != nullptr; }
        TaskHandle_t task()    { return s_task; }

        void pin(uint8_t sig, int32_t value)
        {
            Cmd c;
            c.op = Op::Pin;
            c.sig = sig;
            c.value = value;
            post(c);
        }

        void unpin(uint8_t sig)
        {
            Cmd c;
            c.op = Op::Unpin;
            c.sig = sig;
            post(c);
        }

        void service(uint32_t nowMs)
        {
            if (!s_task) chores(nowMs);
        }

        void test(uint8_t sig, int32_t value, uint32_t forMs)
        {
            Cmd c;
            c.op = Op::Test;
            c.sig = sig;
            c.value = value;
            c.ms = forMs;
            post(c);
        }

        uint32_t testLeftMs(uint8_t sig, uint32_t nowMs)
        {
            const Tests t = s_testsPub.read();
            for (uint8_t i = 0; i < MAX_TESTS; ++i) {
                if (t.sig[i] != sig) continue;
                const int32_t left = (int32_t)(t.untilMs[i] - nowMs);
                return left > 0 ? (uint32_t)left : 0;
            }
            return 0;
        }

        Staged<Vehicle::DecodeTable, true>& decodeTable() { return s_table; }

        Stats stats() { return s_stats.read(); }
    }
}
