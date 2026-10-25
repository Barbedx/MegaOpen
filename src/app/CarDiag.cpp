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
                s_v.intervalS = s_interval.load(std::memory_order_relaxed);
                s_v.state = state;
                s_v.tx = tx;
                s_v.lastAgoMs = s_resultAt ? now - s_resultAt : 0;
                portEXIT_CRITICAL(&s_mux);
            }

            void load()
            {
                Preferences p;
                p.begin("diag", true);
                const bool on = p.getBool("on", false);
                uint8_t iv = p.getUChar("iv", INTERVAL_DEFAULT_S);
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
            LogBus::printf("[diag] climate read %s, every %u s", s_on.load() ? "ON" : "off",
                           (unsigned)s_interval.load());
        }

        void frame(const Frame& f, uint32_t nowMs)
        {
            s_lastFrameMs = nowMs;
            s_anyFrame = true;
            if (!f.extended && f.id == Diag::CLIM_RESP) s_ex.onFrame(f.id, f.data, f.len, nowMs);
        }

        void loop(uint32_t now)
        {
            ::Mcp2518Source& bus = ::Mcp2518Source::instance();
            s_ex.tick(now);
            using S = Diag::Exchange::Status;
            if (s_ex.status() != S::Idle && s_ex.status() != S::Waiting) result(now);

            const uint32_t gen = s_gen.load(std::memory_order_acquire);
            if (gen != s_seenGen) {           // set(): a fresh start
                s_seenGen = gen;
                s_stopped = false;
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
