#include "../Features.h"
#include "CarTask.h"
#include <Preferences.h>
#include "Rt.h"
#include "CarDiag.h"
#include "../vehicle/CanDecode.h"
#include "../vehicle/Derived.h"
#include "../utils/LogBus.h"
#include "../utils/Snapshot.h"
#include "freertos/queue.h"
#include <esp_heap_caps.h>
#include <new>
#include <atomic>
#include <string.h>
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
            Vehicle::Derived   s_derived;
            volatile uint8_t   s_rmLights = Vehicle::Derived::LIGHTS_KMH, s_rmBrake = Vehicle::Derived::BRAKE_KMH;
            // Settings › Gears: written by any task, applied by this one.
            portMUX_TYPE       s_gearMux = portMUX_INITIALIZER_UNLOCKED;
            uint16_t           s_gearSet[6] = {80, 147, 207, 275, 359, 431};
            volatile bool      s_gearDirty = false;

            // The gear learner: steady ratios with the clutch up, binned (2 units
            // a bin, km/h per 1000 rpm x10), sampled at most every 200 ms; the
            // peaks go to the log once a minute ([gear]). This task's own.
            constexpr uint16_t LEARN_BINS = 300;
            constexpr uint32_t LEARN_SAMPLE_MS = 200, LEARN_LOG_MS = 60000;
            uint16_t s_hist[LEARN_BINS] = {};
            int32_t  s_lastRatio = 0;
            uint32_t s_learnAt = 0, s_learnLogAt = 0, s_learnN = 0;

            void learn(const Vehicle::State& st, uint32_t now)
            {
                using namespace Vehicle;
                if ((uint32_t)(now - s_learnAt) < LEARN_SAMPLE_MS) return;
                s_learnAt = now;
                if (!st.has(SIG_SPEED) || !st.has(SIG_RPM)) return;
                const int32_t v = st.get(SIG_SPEED), rpm = st.get(SIG_RPM);
                const bool clutch = st.has(SIG_CLUTCH) && st.get(SIG_CLUTCH);
                const bool neutral = st.has(SIG_NEUTRAL) && st.get(SIG_NEUTRAL);
                const bool rev = st.has(SIG_REVERSE) && st.get(SIG_REVERSE);
                if (v < 1500 || rpm < 1100 || clutch || neutral || rev) { s_lastRatio = 0; return; }
                const int32_t ratio = (int32_t)((int64_t)v * 100 / rpm);
                // Steady: within 3 % of the last sample (no shift, no slip).
                const bool steady = s_lastRatio && (ratio > s_lastRatio ? ratio - s_lastRatio : s_lastRatio - ratio) * 100 <= s_lastRatio * 3;
                s_lastRatio = ratio;
                if (!steady || ratio / 2 >= LEARN_BINS) return;
                if (s_hist[ratio / 2] < 0xFFFF) ++s_hist[ratio / 2];
                ++s_learnN;
            }

            // The peaks: bins (smoothed over 3) above their neighbours within
            // ±8 %, each with at least 25 samples (5 s steady), up to 6.
            // The peaks: bins (smoothed over 3) above their neighbours within
            // ±8 %, each with at least 25 samples (5 s steady), up to max. Any
            // task: the bins are read without a lock (a count off by one at worst).
            uint8_t peaks(uint16_t* ratio, uint32_t* secs, uint8_t max)
            {
                uint8_t found = 0;
                for (uint16_t b = 1; b + 1 < LEARN_BINS && found < max; ++b) {
                    const uint32_t c = (uint32_t)s_hist[b - 1] + s_hist[b] + s_hist[b + 1];
                    if (c < 25) continue;
                    const uint16_t w = (uint16_t)(b * 8 / 100 + 1);
                    bool top = true;
                    for (uint16_t k = b > w ? b - w : 1; k <= b + w && k + 1 < LEARN_BINS && top; ++k) {
                        if (k == b) continue;
                        const uint32_t ck = (uint32_t)s_hist[k - 1] + s_hist[k] + s_hist[k + 1];
                        if (ck > c || (ck == c && k < b)) top = false;
                    }
                    if (!top) continue;
                    ratio[found] = (uint16_t)(b * 2 + 1);
                    secs[found] = c * LEARN_SAMPLE_MS / 1000;
                    ++found;
                }
                return found;
            }

            void learnLog(uint32_t now)
            {
                if ((uint32_t)(now - s_learnLogAt) < LEARN_LOG_MS || s_learnN < 25) return;
                s_learnLogAt = now;
                char out[160];
                int n = 0;
                uint16_t pr[6];
                uint32_t ps[6];
                const uint8_t found = peaks(pr, ps, 6);
                for (uint8_t i = 0; i < found; ++i)
                    n += snprintf(out + n, sizeof(out) - n, " %u (%lu s)", (unsigned)pr[i], (unsigned long)ps[i]);
                uint16_t g[6];
                portENTER_CRITICAL(&s_gearMux);
                memcpy(g, s_gearSet, sizeof(g));
                portEXIT_CRITICAL(&s_gearMux);
                LOGI("gear", "steady ratios seen (km/h per 1000 rpm x10, clutch up):%s; table %u %u %u %u %u %u",
                               found ? out : " none yet", g[0], g[1], g[2], g[3], g[4], g[5]);
            }

            // The odometers come rarely (0x5FD, 0x5C5: at ignition, then per km):
            // a board that starts or reboots after that frame had none for the
            // drive. The last ones are kept in NVS ("odo": km, kmC), saved on a
            // change at most once a minute, and stand in after a boot until the
            // car sends its own.
            constexpr uint32_t ODO_SAVE_MS = 60000;
            int32_t  s_odoSaved = -1, s_odoCSaved = -1;
            uint32_t s_odoAt = 0;

            void svcRestore();

            void odoRestore(uint32_t now)
            {
                if (!s_s.state) return;
                Preferences p;
                p.begin("odo", true);
                const int32_t km = (int32_t)p.getUInt("km", 0), kmC = (int32_t)p.getUInt("kmC", 0);
                p.end();
                s_odoSaved = km;
                s_odoCSaved = kmC;
                if (km && !s_s.state->has(Vehicle::SIG_ODOMETER)) s_s.state->set(Vehicle::SIG_ODOMETER, km, now);
                if (kmC && !s_s.state->has(Vehicle::SIG_ODO_CLUSTER)) s_s.state->set(Vehicle::SIG_ODO_CLUSTER, kmC, now);
                if (km || kmC)
                    LOGI("car", "odometer from NVS: %ld km (cluster %ld km), until the car sends it",
                                   (long)km, (long)kmC);
                svcRestore();
            }

            // service_calc's edge (Derived::Service): saved when it moves, once
            // in 150 km; "odo" svB/svO/svS, svK = known.
            Vehicle::Derived::Service s_svcSaved;

            void svcRestore()
            {
                Preferences p;
                p.begin("odo", true);
                Vehicle::Derived::Service v;
                v.known = p.getBool("svK", false);
                v.base = p.getInt("svB", 0);
                v.odo = p.getInt("svO", 0);
                v.step = p.getInt("svS", 0);
                p.end();
                s_svcSaved = v;
                s_derived.setService(v);
                if (v.known)
                    LOGI("car", "service: edge %ld km at cluster %ld km (step %ld), from NVS",
                                   (long)v.base, (long)v.odo, (long)v.step);
            }

            void svcKeep()
            {
                const Vehicle::Derived::Service& v = s_derived.service();
                if (v == s_svcSaved) return;
                s_svcSaved = v;
                Preferences p;
                p.begin("odo", false);
                p.putBool("svK", v.known);
                p.putInt("svB", v.base);
                p.putInt("svO", v.odo);
                p.putInt("svS", v.step);
                p.end();
                LOGI("car", "service: %s %ld km at cluster %ld km (step %ld)", v.known ? "edge" : "edge lost,",
                               (long)v.base, (long)v.odo, (long)v.step);
            }

            void odoKeep(uint32_t now)
            {
                const Vehicle::State* st = s_s.state;
                if (!st || (uint32_t)(now - s_odoAt) < ODO_SAVE_MS) return;
                const int32_t km = st->has(Vehicle::SIG_ODOMETER) ? st->get(Vehicle::SIG_ODOMETER) : s_odoSaved;
                const int32_t kmC = st->has(Vehicle::SIG_ODO_CLUSTER) ? st->get(Vehicle::SIG_ODO_CLUSTER) : s_odoCSaved;
                if ((km == s_odoSaved || km <= 0) && (kmC == s_odoCSaved || kmC <= 0)) return;
                s_odoAt = now;
                Preferences p;
                p.begin("odo", false);
                if (km > 0 && km != s_odoSaved) { p.putUInt("km", (uint32_t)km); s_odoSaved = km; }
                if (kmC > 0 && kmC != s_odoCSaved) { p.putUInt("kmC", (uint32_t)kmC); s_odoCSaved = kmC; }
                p.end();
            }

            // Every 10 s while the car moves or runs: what the gear and the
            // reminders are computed from, so a drive can be read afterwards.
            void driveLine()
            {
                using namespace Vehicle;
                const State* st = s_s.state;
                if (!st || !st->has(SIG_SPEED)) return;
                const int32_t v = st->get(SIG_SPEED), rpm = st->has(SIG_RPM) ? st->get(SIG_RPM) : 0;
                if (v == 0 && rpm == 0) return;
                auto g = [&](uint8_t s) -> long { return st->has(s) ? (long)st->get(s) : -9; };
                LOGS("drive", "%ld.%02ld km/h %ld rpm ratio %ld gear %ld clutch %ld N %ld R %ld "
                               "lights pos/dip/main %ld/%ld/%ld handbrake %ld; no_lights %ld handbrake_moving %ld; "
                               "odometer %ld (age %ld s), cluster %ld; fuel %ld L (apv %ld) low %ld",
                               (long)(v / 100), (long)(v % 100), (long)rpm, g(SIG_GEAR_RATIO), g(SIG_GEAR),
                               g(SIG_CLUTCH), g(SIG_NEUTRAL), g(SIG_REVERSE), g(SIG_LIGHT_POS),
                               g(SIG_LIGHT_DIPPED), g(SIG_LIGHT_MAIN), g(SIG_HANDBRAKE),
                               g(SIG_DRIVE_NO_LIGHTS), g(SIG_DRIVE_HANDBRAKE), g(SIG_ODOMETER),
                               st->has(SIG_ODOMETER) ? (long)(st->ageMs(SIG_ODOMETER, millis()) / 1000) : -1L,
                               g(SIG_ODO_CLUSTER), g(SIG_FUEL_LEVEL), g(SIG_FUEL_LEVEL_APV), g(SIG_FUEL_LOW));
            }
            // The panel alert rules: httpd stages, this task applies and evaluates.
            Staged<Vehicle::AlertRules> s_alerts;
            Vehicle::AlertEval s_eval;
            uint32_t           s_alertAt = 0, s_shown = 0;
            Snapshot<AlertStats> s_alertPub;
            // The recent alerts, a ring under its own lock (alertsSince()).
            portMUX_TYPE s_logMux = portMUX_INITIALIZER_UNLOCKED;
            AlertEntry   s_log[ALERT_LOG];
            uint32_t     s_logSeq = 0;
            StaticQueue_t      s_cmdBuf;
            uint8_t            s_cmdStore[CMD_DEPTH * sizeof(Cmd)];
            QueueHandle_t      s_cmd = nullptr;
            TaskHandle_t       s_task = nullptr;
            int                s_id = -1;
            std::atomic<uint32_t> s_cmdDrops{0};
            Snapshot<Stats>    s_stats;
            // Two ~2.4 kB buffers, placed in begin(): PSRAM where there is some.
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

            // Vehicle::verified(): measured rows, and no pin (a constant or a
            // test); a derived signal, when every input is. Run after anything
            // that changes either.
            void refreshVerified()
            {
                const Vehicle::DecodeTable& t = s_table.live();
                auto pinned = [](uint8_t s) { return s_s.state && s_s.state->pinned(s); };
                for (uint8_t s = 0; s < Vehicle::SIG_COUNT; ++s) {
                    const uint8_t* in = nullptr;
                    const uint8_t n = Vehicle::Derived::inputs(s, in);
                    bool ok = !pinned(s);
                    if (n) for (uint8_t i = 0; i < n; ++i) ok = ok && t.measured(in[i]) && !pinned(in[i]);
                    else   ok = ok && t.measured(s);
                    Vehicle::setVerified(s, ok);
                }
            }

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
                refreshVerified();
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
                if (changed) refreshVerified();
            }

            // A new table is live: CanDecode reads it in place, the constants
            // that went are unpinned and the new ones pinned.
            void applyTable()
            {
                const Vehicle::DecodeTable& t = s_table.live();
                if (!s_decode.load(t.row, t.nRows)) {
                    // Never keep the old pointer: that buffer is the staging one now.
                    s_decode.load(nullptr, 0);
                    LOGI("car", "decode map REFUSED by CanDecode: no signals decoded");
                }
                // A derived signal with a row or a constant: the binding wins.
                s_derived.clearBound();
                for (uint8_t i = 0; i < t.nRows; ++i) s_derived.setBound(t.row[i].sig, true);
                for (uint8_t i = 0; i < t.nK; ++i)    s_derived.setBound(t.k[i].sig, true);
                if (!s_s.state) { refreshVerified(); return; }
                for (uint8_t i = 0; i < s_nConst; ++i)
                    if (t.constOf(s_constSig[i]) < 0 && !testing(s_constSig[i]))
                        s_s.state->unpin(s_constSig[i]);
                s_nConst = t.nK;
                for (uint8_t i = 0; i < t.nK; ++i) s_constSig[i] = t.k[i].sig;
                pinConsts();
                refreshVerified();
            }

            // The panel alert rules, every ALERT_MS. One popup pending at a time
            // (AlertEval): it waits for the glass, and goes if its condition
            // clears first.
            void alerts(uint32_t now)
            {
                s_alerts.applyIfDirty();
                if ((uint32_t)(now - s_alertAt) < ALERT_MS) return;
                s_alertAt = now;
                const Vehicle::AlertRules& r = s_alerts.live();
                s_eval.tick(r, *s_s.state, now);
                Vehicle::AlertEval::Fire f;
                const bool free = !s_s.glassFree || s_s.glassFree();
                if (s_eval.take(r, *s_s.state, free, f)) {
                    char text[48];
                    Vehicle::AlertEval::text(text, sizeof(text), f.sig, f.value, Vehicle::verified(f.sig));
                    if (s_s.alert) s_s.alert("Car alert", text);
                    ++s_shown;
                    LOGI("alert", "rule %u: %s", (unsigned)(f.row + 1), text);
                    AlertEntry e;
                    e.ms = now;
                    e.sig = f.sig;
                    e.row = f.row;
                    e.value = f.value;
                    memcpy(e.text, text, sizeof(e.text));
                    portENTER_CRITICAL(&s_logMux);
                    e.seq = ++s_logSeq;
                    s_log[(e.seq - 1) % ALERT_LOG] = e;
                    portEXIT_CRITICAL(&s_logMux);
                }
                AlertStats st;
                st.fired = s_eval.fired();
                st.shown = s_shown;
                st.overwritten = s_eval.overwritten();
                st.dropped = s_eval.dropped();
                s_alertPub.publish(st);
            }

            // The owner's chores between frames: commands, a staged table,
            // tests running out, the alert rules.
            void chores(uint32_t now)
            {
                if (!s_s.state) return;
                Cmd c;
                while (s_cmd && xQueueReceive(s_cmd, &c, 0) == pdTRUE) applyCmd(c, now);
                if (s_table.applyIfDirty()) applyTable();
                expireTests(now);
                alerts(now);
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
                configASSERT(m);   // setup, ~2.4 kB: there is no running without it
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
                LOGI("car", "decode map: %s, %u rows, %u constants, in %s", fromNvs ? "NVS" :
                               n ? "factory (the NVS blob did NOT unpack)" : "factory",
                               (unsigned)t.nRows, (unsigned)t.nK, ps ? "PSRAM" : "DRAM");
            }

            void frame(const Frame& f, uint32_t now)
            {
                if (s_s.state && s_decode.apply(f, *s_s.state, now)) {
                    s_derived.setReminderSpeeds(s_rmLights, s_rmBrake);
                    if (s_gearDirty) {
                        Vehicle::Derived::Gears gs;
                        portENTER_CRITICAL(&s_gearMux);
                        for (int i = 0; i < 6; ++i) gs.ratio[i] = s_gearSet[i];
                        s_gearDirty = false;
                        portEXIT_CRITICAL(&s_gearMux);
                        s_derived.setGears(gs);
                    }
                    s_derived.update(*s_s.state, now);
                    learn(*s_s.state, now);
                    odoKeep(now);
                    svcKeep();
                }
#if FEATURE_DIAG
                CarDiag::frame(f, now);      // the climate's reply, and the bus is alive
#endif
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
                LOGS("car", "fps=%lu frames=%lu drainMax=%lu decode=%lu/%lu short=%lu "
                               "busyMax=%luus cmdDrops=%lu src %s",
                               (unsigned long)s_fps, (unsigned long)s_frames,
                               (unsigned long)s_drainMax, (unsigned long)s_decode.hits(),
                               (unsigned long)s_decode.unknown(), (unsigned long)s_decode.short_(),
                               (unsigned long)s_busyMaxUs,
                               (unsigned long)s_cmdDrops.load(std::memory_order_relaxed), src);
                s_drainMax = 0;
                s_busyMaxUs = 0;
                driveLine();
                learnLog(millis());
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
                odoRestore(millis());
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

#if FEATURE_DIAG
                    CarDiag::loop(now);          // after the frames: their replies are in
#endif
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
            s_alerts.init(s.alerts);
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
            LOGI("car", "task: %u source(s), %u decode rows", (unsigned)s_s.n,
                           (unsigned)s_decode.rows());
        }

        bool         running() { return s_task != nullptr; }
        TaskHandle_t task()    { return s_task; }

        uint8_t gearPeaks(uint16_t* ratio, uint32_t* secs, uint8_t max) { return peaks(ratio, secs, max); }

        void setGears(const uint16_t ratio[6])
        {
            portENTER_CRITICAL(&s_gearMux);
            const bool same = !memcmp(s_gearSet, ratio, sizeof(s_gearSet));
            if (!same) {
                memcpy(s_gearSet, ratio, sizeof(s_gearSet));
                s_gearDirty = true;
            }
            portEXIT_CRITICAL(&s_gearMux);
        }

        void setReminderSpeeds(uint8_t lightsKmh, uint8_t brakeKmh)
        {
            s_rmLights = lightsKmh;
            s_rmBrake = brakeKmh;
        }

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
        Staged<Vehicle::AlertRules>& alertRules() { return s_alerts; }
        AlertStats alertStats() { return s_alertPub.read(); }

        void injectAlert(uint8_t sig, int32_t value, const char* text)
        {
            AlertEntry e;
            e.ms = millis();
            e.sig = sig;
            e.row = 0xFF;                  // no rule: a test
            e.value = value;
            snprintf(e.text, sizeof(e.text), "%s", text ? text : "");
            portENTER_CRITICAL(&s_logMux);
            e.seq = ++s_logSeq;
            s_log[(e.seq - 1) % ALERT_LOG] = e;
            portEXIT_CRITICAL(&s_logMux);
            LOGI("alert", "test for the apps: %s", e.text);
        }

        uint8_t alertsSince(uint32_t since, AlertEntry* out, uint8_t cap, uint32_t& last)
        {
            uint8_t n = 0;
            portENTER_CRITICAL(&s_logMux);
            last = s_logSeq;
            const uint32_t oldest = s_logSeq > ALERT_LOG ? s_logSeq - ALERT_LOG + 1 : 1;
            for (uint32_t q = since + 1 < oldest ? oldest : since + 1; q <= s_logSeq && n < cap; ++q)
                out[n++] = s_log[(q - 1) % ALERT_LOG];
            portEXIT_CRITICAL(&s_logMux);
            return n;
        }
    }
}
