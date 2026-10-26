#include "CarDiag.h"
#if FEATURE_DIAG
#include <Arduino.h>
#include <Preferences.h>
#include <atomic>
#include "../bus/Mcp2518Source.h"
#include "../diag/ClimDiag.h"
#include "../utils/LogBus.h"
#include "../vehicle/VehicleState.h"

namespace App
{
    namespace CarDiag
    {
        namespace
        {
            constexpr uint32_t SUMMARY_MS = 60000;   // the [diag] line while polling
            const uint8_t REQ[] = {0x21, 0xC1};

            bool sendFrame(uint32_t id, const uint8_t* d, uint8_t len, void*)
            {
                return ::Mcp2518Source::instance().send(id, d, len);
            }

            Vehicle::State* s_state = nullptr;
            Diag::Exchange  s_ex(sendFrame, nullptr);

            // The switches: any task writes, the car task reads.
            std::atomic<bool>     s_on{false};
            std::atomic<uint8_t>  s_interval{INTERVAL_DEFAULT_S};
            std::atomic<uint32_t> s_gen{0};          // bumped by set(): clears a stop
            std::atomic<bool>     s_probe{false};
            std::atomic<bool>     s_sweepReq{false};

            // ---- the sweep (the car task's) ----
            struct Step { uint8_t req[2]; const char* what; };
            // Every read DDT lists, without a session, then in 10 C0 with a
            // tester present every two reads (the session would lapse), then
            // back to the default session, always.
            const Step SWEEP[] = {
                {{0x21, 0x80}, "21 80, no session"},
                {{0x21, 0xC0}, "21 C0, no session"},
                {{0x21, 0xC1}, "21 C1, no session"},
                {{0x21, 0xC2}, "21 C2, no session"},
                {{0x21, 0x90}, "21 90 (the driver's setpoint?), no session"},
                {{0x21, 0xA0}, "21 A0, no session"},
                {{0x10, 0xC0}, "10 C0, open the extended session"},
                {{0x21, 0x80}, "21 80 in the session"},
                {{0x21, 0xC0}, "21 C0 in the session"},
                {{0x3E, 0x01}, "3E 01, tester present"},
                {{0x21, 0xC1}, "21 C1 in the session"},
                {{0x21, 0xC2}, "21 C2 in the session"},
                {{0x3E, 0x01}, "3E 01, tester present"},
                {{0x21, 0x90}, "21 90 in the session"},
                {{0x21, 0xA0}, "21 A0 in the session"},
                {{0x10, 0x81}, "10 81, back to the default session"},
            };
            constexpr uint8_t  SWEEP_N = sizeof(SWEEP) / sizeof(SWEEP[0]);
            constexpr uint32_t SWEEP_GAP_MS = 1000;   // between steps: the session holds ~5 s

            // ---- the session for the polling read (diag.session) ----
            enum class Pend : uint8_t { Read, Session, Tester, End };
            constexpr uint32_t TESTER_MS = 2000;
            std::atomic<bool> s_session{false};
            Pend     s_pend = Pend::Read;
            bool     s_inSes = false, s_sesRefused = false;
            uint32_t s_tpAt = 0;
            int8_t   s_sweep = -1;           // the step waiting or next; -1 none
            bool     s_sweepSent = false;     // that step's request is out
            uint32_t s_sweepAt = 0;
            uint16_t s_sweepIds0 = 0, s_sweepIdsMin = 0;

            // The bus a second at a time, for the sweep's lines: frames and
            // distinct standard ids. A cluster that reboots drops its ids.
            uint8_t  s_idBits[256] = {};
            uint32_t s_winFrames = 0, s_winStart = 0;
            uint16_t s_busIds = 0, s_busFps = 0;

            // The car task's own.
            uint32_t s_seenGen = 0;
            uint32_t s_lastFrameMs = 0;
            bool     s_anyFrame = false;
            uint32_t s_nextAt = 0, s_summaryAt = 0, s_resultAt = 0;
            bool     s_stopped = false, s_probing = false, s_backoff = false;
            uint8_t  s_timeoutsInRow = 0;

            portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
            View s_v;                                // under s_mux

            void hex(char* out, size_t cap, const uint8_t* b, uint8_t n)
            {
                size_t k = 0;
                out[0] = 0;
                for (uint8_t i = 0; i < n && k + 4 < cap; ++i) k += snprintf(out + k, cap - k, "%s%02X", i ? " " : "", b[i]);
            }

            void result(uint32_t now)
            {
                using S = Diag::Exchange::Status;
                const S st = s_ex.status();
                if (s_pend != Pend::Read) {
                    const Pend what = s_pend;
                    s_pend = Pend::Read;
                    char why[24];
                    if (st == S::Negative) snprintf(why, sizeof(why), "7F %02X %02X", s_ex.reply()[1], s_ex.nrc());
                    else snprintf(why, sizeof(why), "%s", Diag::Exchange::statusId(st));
                    if (what == Pend::Session) {
                        if (st == S::Ok) {
                            s_inSes = true;
                            s_tpAt = now;
                            LogBus::line("[diag] session 10 C0: open (50 C0); tester present every 2 s");
                        } else {
                            s_sesRefused = true;
                            LogBus::printf("W [diag] session 10 C0: %s; polling goes on without it", why);
                        }
                    } else if (what == Pend::Tester) {
                        if (st == S::Ok) s_tpAt = now;
                        else {
                            s_inSes = false;
                            LogBus::printf("W [diag] tester present 3E 01: %s; the session is gone, opened again "
                                           "before the next read", why);
                        }
                    } else {
                        LogBus::printf("[diag] session closed (10 81): %s", why);
                    }
                    s_ex.clear();
                    return;
                }
                Diag::ClimValues cv;
                const bool good = st == S::Ok && Diag::decode21C1(s_ex.reply(), s_ex.replyLen(), cv);
                char b[3 * 24 + 1];
                hex(b, sizeof(b), s_ex.reply(), s_ex.replyLen() < 24 ? s_ex.replyLen() : 24);

                portENTER_CRITICAL(&s_mux);
                s_v.last = Diag::Exchange::statusId(st);
                s_v.nrc = s_ex.nrc();
                s_v.replyLen = s_ex.replyLen() < sizeof(s_v.reply) ? s_ex.replyLen() : sizeof(s_v.reply);
                memcpy(s_v.reply, s_ex.reply(), s_v.replyLen);
                if (good) { ++s_v.ok; s_v.cabinX10 = cv.cabinX10; s_v.sunWm2 = cv.sunWm2; s_v.haveValues = true; }
                else if (st == S::Timeout) ++s_v.timeouts;
                else if (st == S::Negative) ++s_v.negatives;
                else ++s_v.bad;
                portEXIT_CRITICAL(&s_mux);
                s_resultAt = now;

                if (good) {
                    s_timeoutsInRow = 0;
                    s_backoff = false;
                    if (s_state) {
                        s_state->set(Vehicle::SIG_CABIN_TEMP, cv.cabinX10, now);
                        s_state->set(Vehicle::SIG_CLIM_SUN, cv.sunWm2, now);
                    }
                } else if (st == S::Timeout) {
                    if (++s_timeoutsInRow >= 3) {
                        s_nextAt = now + BACKOFF_MS;
                        s_timeoutsInRow = 0;
                        s_backoff = true;
                        LogBus::printf("[diag] climate: no answer 3 times, next try in %u s",
                                       (unsigned)(BACKOFF_MS / 1000));
                    }
                } else if (st == S::Negative) {
                    // The ECU said no: asking again changes nothing.
                    s_stopped = true;
                }

                // A probe, and the first result of a polling run or any
                // failure, says everything; a good poll only once a minute.
                if (s_probing || !good || s_summaryAt == 0 || (uint32_t)(now - s_summaryAt) >= SUMMARY_MS) {
                    if (good)
                        LogBus::printf("[diag] climate 21C1: cabin %d.%d C, evaporator %d.%d C, sun %u W/m2, "
                                       "outside %d C, coolant %d C, battery %u.%02u V; reply %s",
                                       cv.cabinX10 / 10, abs(cv.cabinX10 % 10), cv.evapX10 / 10, abs(cv.evapX10 % 10),
                                       (unsigned)cv.sunWm2, cv.outsideC, cv.coolantC, (unsigned)(cv.battX100 / 100),
                                       (unsigned)(cv.battX100 % 100), b);
                    else if (st == S::Negative)
                        LogBus::printf("W [diag] climate 21C1 refused: 7F 21 %02X%s; polling stopped", s_ex.nrc(),
                                       s_ex.nrc() == 0x80 || s_ex.nrc() == 0x7F ? " (not in this session)" : "");
                    else
                        LogBus::printf("W [diag] climate 21C1: %s%s%s", Diag::Exchange::statusId(st),
                                       s_ex.replyLen() ? "; reply " : "", s_ex.replyLen() ? b : "");
                    if (!s_probing) s_summaryAt = now;
                }
                s_probing = false;
                s_ex.clear();
            }

            void publish(uint32_t now, const char* state)
            {
                const bool tx = ::Mcp2518Source::instance().transmitting();
                portENTER_CRITICAL(&s_mux);
                s_v.on = s_on.load(std::memory_order_relaxed);
                s_v.session = s_session.load(std::memory_order_relaxed);
                s_v.inSession = s_inSes;
                s_v.intervalS = s_interval.load(std::memory_order_relaxed);
                s_v.state = state;
                s_v.tx = tx;
                s_v.lastAgoMs = s_resultAt ? now - s_resultAt : 0;
                portEXIT_CRITICAL(&s_mux);
            }

            // The sweep, while one runs: true when it owns this pass.
            bool sweepLoop(uint32_t now, ::Mcp2518Source& bus)
            {
                using S = Diag::Exchange::Status;
                if (s_sweep < 0) {
                    if (!s_sweepReq.load()) return false;
                    if (s_ex.status() == S::Waiting) return false;      // a poll first
                    s_sweepReq.store(false);
                    if (!s_anyFrame || (uint32_t)(now - s_lastFrameMs) >= BUS_QUIET_MS) {
                        LogBus::line("W [diag] sweep: the car bus is quiet (ignition off?), nothing sent");
                        return false;
                    }
                    if (!bus.transmitting() && !bus.setTransmit(true, now)) {
                        LogBus::line("W [diag] sweep: the controller would not transmit (rate not locked), nothing sent");
                        return false;
                    }
                    s_ex.clear();
                    s_pend = Pend::Read;
                    s_sweep = 0;
                    s_sweepSent = false;
                    s_sweepAt = now + SWEEP_GAP_MS;
                    s_sweepIds0 = s_busIds;
                    s_sweepIdsMin = s_busIds;
                    LogBus::printf("[diag] sweep: %u steps to 0x744; bus before: %u fps, %u ids", (unsigned)SWEEP_N,
                                   (unsigned)s_busFps, (unsigned)s_busIds);
                    publish(now, "sweep");
                    return true;
                }
                const Step& st = SWEEP[s_sweep];
                if (!s_sweepSent) {
                    if ((int32_t)(now - s_sweepAt) < 0) return true;
                    if (bus.txFault()) {
                        LogBus::printf("W [diag] sweep: bus fault before step %d, stopped (bus %u fps, %u ids)",
                                       s_sweep + 1, (unsigned)s_busFps, (unsigned)s_busIds);
                        s_sweep = -1;
                        return true;
                    }
                    LogBus::printf("[diag] sweep %d/%u: %s; bus %u fps, %u ids", s_sweep + 1, (unsigned)SWEEP_N, st.what,
                                   (unsigned)s_busFps, (unsigned)s_busIds);
                    s_ex.start(st.req, 2, now);
                    s_sweepSent = true;
                    portENTER_CRITICAL(&s_mux);
                    ++s_v.sent;
                    portEXIT_CRITICAL(&s_mux);
                    return true;
                }
                if (s_ex.status() == S::Waiting) return true;
                char b[3 * 24 + 1];
                hex(b, sizeof(b), s_ex.reply(), s_ex.replyLen() < 24 ? s_ex.replyLen() : 24);
                const S r = s_ex.status();
                if (r == S::Negative)
                    LogBus::printf("W [diag] sweep %d/%u: %02X %02X -> 7F %02X %02X (refused)", s_sweep + 1,
                                   (unsigned)SWEEP_N, st.req[0], st.req[1], st.req[0], s_ex.nrc());
                else
                    LogBus::printf("%s[diag] sweep %d/%u: %02X %02X -> %s%s%s", r == S::Ok ? "" : "W ", s_sweep + 1,
                                   (unsigned)SWEEP_N, st.req[0], st.req[1], Diag::Exchange::statusId(r),
                                   s_ex.replyLen() ? ", reply " : "", s_ex.replyLen() ? b : "");
                Diag::Clim2190 d90;
                if (r == S::Ok && Diag::decode2190(s_ex.reply(), s_ex.replyLen(), d90))
                    LogBus::printf("[diag] sweep: 2190 setpoint %u.%u C, blower %u, distribution %u, recirc %u, "
                                   "A/C %u (raw codes, Clim_X84_SERIE)", (unsigned)(d90.setX10 / 10),
                                   (unsigned)(d90.setX10 % 10), (unsigned)d90.blower, (unsigned)d90.dist,
                                   (unsigned)d90.recirc, (unsigned)d90.ac);
                Diag::ClimValues cv;
                if (r == S::Ok && Diag::decode21C1(s_ex.reply(), s_ex.replyLen(), cv)) {
                    LogBus::printf("[diag] sweep: cabin %d.%d C, sun %u W/m2", cv.cabinX10 / 10, abs(cv.cabinX10 % 10),
                                   (unsigned)cv.sunWm2);
                    if (s_state) {
                        s_state->set(Vehicle::SIG_CABIN_TEMP, cv.cabinX10, now);
                        s_state->set(Vehicle::SIG_CLIM_SUN, cv.sunWm2, now);
                    }
                }
                portENTER_CRITICAL(&s_mux);
                s_v.last = Diag::Exchange::statusId(r);
                s_v.nrc = s_ex.nrc();
                s_v.replyLen = s_ex.replyLen() < sizeof(s_v.reply) ? s_ex.replyLen() : sizeof(s_v.reply);
                memcpy(s_v.reply, s_ex.reply(), s_v.replyLen);
                portEXIT_CRITICAL(&s_mux);
                s_resultAt = now;
                s_ex.clear();
                ++s_sweep;
                s_sweepSent = false;
                s_sweepAt = now + SWEEP_GAP_MS;
                if (s_sweep >= SWEEP_N) {
                    LogBus::printf("[diag] sweep done; bus now %u fps, %u ids (before %u, lowest during %u)",
                                   (unsigned)s_busFps, (unsigned)s_busIds, (unsigned)s_sweepIds0,
                                   (unsigned)s_sweepIdsMin);
                    s_sweep = -1;
                    s_inSes = false;            // the sweep ended with 10 81
                    s_nextAt = now + SWEEP_GAP_MS;
                }
                return true;
            }

            void load()
            {
                Preferences p;
                p.begin("diag", true);
                const bool on = p.getBool("on", false);
                uint8_t iv = p.getUChar("iv", INTERVAL_DEFAULT_S);
                s_session.store(p.getBool("ses", false));
                p.end();
                if (iv < INTERVAL_MIN_S || iv > INTERVAL_MAX_S) iv = INTERVAL_DEFAULT_S;
                s_on.store(on);
                s_interval.store(iv);
            }
        }

        void begin(Vehicle::State* state)
        {
            s_state = state;
            load();
            LogBus::printf("[diag] climate read %s, every %u s%s", s_on.load() ? "ON" : "off",
                           (unsigned)s_interval.load(), s_session.load() ? ", in a 10 C0 session" : "");
        }

        void frame(const Frame& f, uint32_t nowMs)
        {
            s_lastFrameMs = nowMs;
            s_anyFrame = true;
            ++s_winFrames;
            if (!f.extended && f.id < 2048) s_idBits[f.id >> 3] |= (uint8_t)(1u << (f.id & 7));
            if (!f.extended && f.id == Diag::CLIM_RESP) s_ex.onFrame(f.id, f.data, f.len, nowMs);
        }

        void loop(uint32_t now)
        {
            ::Mcp2518Source& bus = ::Mcp2518Source::instance();
            if ((uint32_t)(now - s_winStart) >= 1000) {
                uint16_t ids = 0;
                for (uint8_t b : s_idBits) ids = (uint16_t)(ids + __builtin_popcount(b));
                s_busIds = ids;
                s_busFps = (uint16_t)(s_winFrames * 1000u / (uint32_t)(now - s_winStart ? now - s_winStart : 1));
                memset(s_idBits, 0, sizeof(s_idBits));
                s_winFrames = 0;
                s_winStart = now;
                if (s_sweep >= 0 && ids < s_sweepIdsMin) s_sweepIdsMin = ids;
            }
            s_ex.tick(now);
            using S = Diag::Exchange::Status;
            if (sweepLoop(now, bus)) return;
            if (s_ex.status() != S::Idle && s_ex.status() != S::Waiting) result(now);

            const uint32_t gen = s_gen.load(std::memory_order_acquire);
            if (gen != s_seenGen) {           // set(): a fresh start
                s_seenGen = gen;
                s_stopped = false;
                s_sesRefused = false;
                s_timeoutsInRow = 0;
                s_backoff = false;
                s_summaryAt = 0;
                s_nextAt = now;
                bus.clearTxFault();
            }
            const bool on = s_on.load(std::memory_order_relaxed);
            const bool probe = s_probe.load(std::memory_order_relaxed);
            const bool waiting = s_ex.status() == S::Waiting;

            if (!on && !probe && !waiting) {
                if (s_inSes && bus.transmitting() && s_anyFrame && (uint32_t)(now - s_lastFrameMs) < BUS_QUIET_MS) {
                    static const uint8_t END[] = {0x10, 0x81};
                    s_inSes = false;
                    s_pend = Pend::End;
                    s_ex.start(END, sizeof(END), now);
                    return publish(now, "off");
                }
                s_inSes = false;
                if (bus.transmitting()) bus.setTransmit(false, now);
                return publish(now, "off");
            }
            if (bus.txFault()) {
                s_probe.store(false);
                return publish(now, "fault");
            }
            if (!s_anyFrame || (uint32_t)(now - s_lastFrameMs) >= BUS_QUIET_MS) {
                // The car is off: our frame would get no ACK. A probe asked now is
                // dropped, not kept for whenever the bus wakes.
                s_probe.store(false);
                if (!waiting && bus.transmitting()) bus.setTransmit(false, now);
                return publish(now, "quiet");
            }
            if (waiting) return publish(now, "waiting");

            // diag.session: open 10 C0 before the read, keep it with 3E 01.
            if (on && !probe && !s_stopped && s_session.load(std::memory_order_relaxed) && !s_sesRefused &&
                (!s_inSes ? (int32_t)(now - s_nextAt) >= 0 : (uint32_t)(now - s_tpAt) >= TESTER_MS)) {
                if (!bus.transmitting() && !bus.setTransmit(true, now)) {
                    s_nextAt = now + 5000;
                    return publish(now, "refused");
                }
                static const uint8_t OPEN[] = {0x10, 0xC0}, TP[] = {0x3E, 0x01};
                s_pend = s_inSes ? Pend::Tester : Pend::Session;
                if (s_inSes) s_tpAt = now;
                s_ex.start(s_inSes ? TP : OPEN, 2, now);
                return publish(now, "waiting");
            }
            if (!s_session.load(std::memory_order_relaxed)) s_inSes = false;
            const bool due = probe || (on && !s_stopped && (int32_t)(now - s_nextAt) >= 0);
            if (!due) {
                // A probe done with polling off: back to listen-only.
                if (!on && bus.transmitting()) bus.setTransmit(false, now);
                return publish(now, s_stopped ? "stopped" : s_backoff ? "backoff" : "idle");
            }
            if (!bus.transmitting() && !bus.setTransmit(true, now)) {
                s_probe.store(false);
                s_nextAt = now + 5000;
                return publish(now, "refused");   // the rate is not locked yet
            }
            s_probing = probe;
            s_probe.store(false);
            s_pend = Pend::Read;
            if (s_ex.start(REQ, sizeof(REQ), now)) {
                portENTER_CRITICAL(&s_mux);
                ++s_v.sent;
                portEXIT_CRITICAL(&s_mux);
            }
            if ((int32_t)(s_nextAt - now) <= 0)
                s_nextAt = now + (uint32_t)s_interval.load(std::memory_order_relaxed) * 1000u;
            publish(now, "waiting");
        }

        const char* set(bool on, uint8_t intervalS)
        {
            if (intervalS < INTERVAL_MIN_S || intervalS > INTERVAL_MAX_S) return "param interval";
            Preferences p;
            if (!p.begin("diag", false)) return "nvs";
            p.putBool("on", on);
            p.putUChar("iv", intervalS);
            p.end();
            s_interval.store(intervalS);
            s_on.store(on);
            s_gen.fetch_add(1, std::memory_order_release);
            LogBus::printf("[diag] climate read %s, every %u s", on ? "ON" : "off", (unsigned)intervalS);
            return nullptr;
        }

        const char* setSession(bool on)
        {
            Preferences p;
            if (!p.begin("diag", false)) return "nvs";
            p.putBool("ses", on);
            p.end();
            s_session.store(on);
            s_gen.fetch_add(1, std::memory_order_release);
            LogBus::printf("[diag] polling in a 10 C0 session: %s", on ? "ON" : "off");
            return nullptr;
        }

        void sweep()
        {
            s_gen.fetch_add(1, std::memory_order_release);
            s_sweepReq.store(true);
        }

        void probe()
        {
            s_gen.fetch_add(1, std::memory_order_release);
            s_probe.store(true);
        }

        void view(View& out)
        {
            portENTER_CRITICAL(&s_mux);
            out = s_v;
            portEXIT_CRITICAL(&s_mux);
        }
    }
}
#endif
