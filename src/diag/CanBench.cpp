#include "CanBench.h"
#if FEATURE_DISPLAY_CAN || FEATURE_VEHICLE_CAN
#include "../app/RequestReply.h"
#include "../board/BoardProfile.h"
#include "../utils/LogBus.h"
#include "../utils/Snapshot.h"
#include "freertos/queue.h"

namespace Diag
{
    namespace CanBench
    {
        namespace
        {
            enum Op : uint8_t { OP_MODE = 1, OP_LISTEN, OP_TIMING, OP_SELFTEST, OP_LOOP,
                                OP_TXPULLS, OP_PINTEST, OP_CRXPULLS };

            constexpr uint8_t  RR_DEPTH  = 4;
            constexpr uint32_t SAMPLE_MS = 100;
            constexpr uint32_t ALERT_BUCKET_MS = 1000;

            Setup              s_s;
            StaticQueue_t      s_rrBuf;
            uint8_t            s_rrStore[RR_DEPTH * sizeof(RR::Ticket)];
            QueueHandle_t      s_rrQ = nullptr;
            RR::Channel        s_rr;

            Snapshot<CanStatus> s_status;
            twai_status_info_t  s_info{};
            bool                s_infoValid = false;
            uint32_t            s_seq = 0;
            uint32_t            s_sampleAtMs = 0;
            // Alerts in two one-second buckets, so a reader always sees at least
            // the last full second.
            uint32_t            s_alertsNow = 0, s_alertsPrev = 0, s_bucketAtMs = 0;

            // One writer each (the owner); httpd reads the words.
            volatile bool s_listenOnly = true;
            volatile int  s_loopRec = -1, s_loopDom = -1;

            int8_t rxPin() { return Board::PIN_CAN_RX; }
            int8_t txPin() { return Board::PIN_CAN_TX; }

            // Give the pins back to the controller after a diagnostic bit-banged
            // them, in the role's mode — never promote a listen-only (vehicle)
            // node to an ACKing one, nor a display node the web switched to
            // listen-only (the ui would suppress every render while the
            // controller transmitted sync/hello).
            void twaiRestartInRole(int8_t tx, int8_t rx)
            {
                const twai_mode_t mode = (s_s.role == Bus::Role::Vehicle || s_listenOnly)
                                             ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL;
                TwaiBus::instance().begin((gpio_num_t)tx, (gpio_num_t)rx, 500000, mode);
                TwaiBus::instance().onReceive(s_s.rx, nullptr);
                TwaiBus::instance().onTransmit(s_s.txTap);
            }

            // Transceiver checks. A transceiver echoes the bus on its receiver:
            //   CTX high -> bus recessive                -> CRX must read 1
            //   CTX low  -> chip drives the bus dominant -> CRX must read 0
            // On demand only: a dominant destroys any frame in flight.
            //
            // Toggled loopback (/api/can/loop). Toggled, never held: a held
            // dominant can disarm this bench's transceiver within ~1 ms, so
            // selfTestNow()'s 300 us hold can read "broken" on a working module.
            // CTX low 50 us, sample CRX at 30 us, high 50 us; 400 cycles in
            // bursts with a yield. Restored in the role's mode.
            void loopTestNow(uint32_t& cycles, uint32_t& followed)
            {
                cycles = followed = 0;
                TwaiBus::instance().end();
                const int8_t rx = rxPin();
                const int8_t tx = txPin();
                if (rx < 0 || tx < 0) return;
                pinMode(rx, INPUT);
                pinMode(tx, OUTPUT);
                digitalWrite(tx, HIGH);
                delayMicroseconds(200);
                for (uint8_t burst = 0; burst < 8; ++burst) {
                    for (uint8_t i = 0; i < 50; ++i) {
                        digitalWrite(tx, LOW);
                        delayMicroseconds(30);
                        if (digitalRead(rx) == LOW) ++followed;
                        delayMicroseconds(20);
                        digitalWrite(tx, HIGH);
                        delayMicroseconds(50);
                        ++cycles;
                    }
                    delay(2);
                }
                digitalWrite(tx, HIGH);
                pinMode(tx, INPUT);
                twaiRestartInRole(tx, rx);
                LogBus::printf("[can] loop test: CTX toggled %u times, CRX followed %u",
                               (unsigned)cycles, (unsigned)followed);
            }

            bool selfTestNow()
            {
                TwaiBus::instance().end();             // release the pins from the controller

                // The configured pins, not the compiled defaults.
                const int8_t rx = rxPin();
                const int8_t tx = txPin();
                if (rx < 0 || tx < 0) return false;

                pinMode(rx, INPUT);
                pinMode(tx, OUTPUT);
                digitalWrite(tx, HIGH);
                delayMicroseconds(300);
                const int rec = digitalRead(rx);
                digitalWrite(tx, LOW);
                delayMicroseconds(300);
                const int dom = digitalRead(rx);
                digitalWrite(tx, HIGH);
                delayMicroseconds(300);
                pinMode(tx, INPUT);

                s_loopRec = rec;
                s_loopDom = dom;

                twaiRestartInRole(tx, rx);

                const bool ok = (rec == 1 && dom == 0);
                LogBus::printf("[can] self-test: CTX=1->CRX=%d (want 1), CTX=0->CRX=%d (want 0) => TX path %s",
                               rec, dom, ok ? "OK" : "BROKEN");
                return ok;
            }

            // TX pad under float / pull-up / pull-down, off the driver. Pull-up
            // reads 0: something holds it low against ~45k.
            bool txPullsNow(int& fl, int& up, int& down)
            {
                const int8_t rx = rxPin();
                const int8_t tx = txPin();
                if (rx < 0 || tx < 0) return false;
                TwaiBus::instance().end();
                pinMode(tx, INPUT);          delayMicroseconds(200);
                fl = digitalRead(tx);
                pinMode(tx, INPUT_PULLUP);   delayMicroseconds(500);
                up = digitalRead(tx);
                pinMode(tx, INPUT_PULLDOWN); delayMicroseconds(500);
                down = digitalRead(tx);
                pinMode(tx, INPUT);
                twaiRestartInRole(tx, rx);
                return true;
            }

            // Each pad driven high and low as a GPIO and read back, off the
            // driver; left recessive (high), never dominant.
            bool pinTestNow(bool& rxOk, bool& txOk)
            {
                const int8_t rx = rxPin();
                const int8_t tx = txPin();
                if (rx < 0 || tx < 0) return false;
                TwaiBus::instance().end();
                auto test = [](int pin) {
                    pinMode(pin, OUTPUT);
                    digitalWrite(pin, HIGH);
                    delayMicroseconds(200);
                    const int hi = digitalRead(pin);
                    digitalWrite(pin, LOW);
                    delayMicroseconds(200);
                    const int lo = digitalRead(pin);
                    digitalWrite(pin, HIGH);
                    delayMicroseconds(200);
                    pinMode(pin, INPUT);
                    return (hi == 1 && lo == 0);
                };
                rxOk = test(rx);
                txOk = test(tx);
                twaiRestartInRole(tx, rx);
                return true;
            }

            // CRX under the internal pulls. Input-side only: the controller
            // keeps its RX routing and its TX pad, so no reinstall.
            void crxPullsNow(int& raw, int& up, int& down, int& ctxRaw)
            {
                const int8_t rx = rxPin();
                const int8_t tx = txPin();
                raw = up = down = ctxRaw = -1;
                if (rx < 0 || tx < 0) return;
                raw = digitalRead(rx);
                pinMode(rx, INPUT_PULLUP);
                delayMicroseconds(200);
                up = digitalRead(rx);
                pinMode(rx, INPUT_PULLDOWN);
                delayMicroseconds(200);
                down = digitalRead(rx);
                pinMode(rx, INPUT);
                ctxRaw = digitalRead(tx);
            }

            RR::Result run(const RR::Args& a)
            {
                RR::Result r;
                TwaiBus& bus = TwaiBus::instance();
                switch (a.op) {
                    case OP_MODE:
                        r.status = bus.setMode((twai_mode_t)a.u[0]) ? 1 : 0;
                        // Keep listenOnly() (the ui's render gate, and the mode a
                        // bench op restores) in step with the controller.
                        if (r.status == 1)
                            s_listenOnly = (twai_mode_t)a.u[0] == TWAI_MODE_LISTEN_ONLY;
                        break;
                    case OP_LISTEN: {
                        const bool on = a.u[0] != 0;
                        s_listenOnly = on;
                        r.status = bus.setMode(on ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL) ? 1 : 0;
                        LogBus::printf("[can] listen-only = %s", on ? "ON" : "off (TX allowed)");
                        break;
                    }
                    case OP_TIMING: {
                        TwaiBus::Timing t{};
                        t.brp = a.u[0]; t.tseg1 = (uint8_t)a.u[1];
                        t.tseg2 = (uint8_t)a.u[2]; t.sjw = (uint8_t)a.u[3];
                        bus.setTiming(t);
                        // restart(), not setMode(): the timing is only read at install.
                        r.status = bus.restart() ? 1 : 0;
                        r.u[0] = bus.samplePointTenths();
                        r.u[1] = bus.effectiveBitrate();
                        break;
                    }
                    case OP_SELFTEST:
                        r.status = selfTestNow() ? 1 : 0;
                        r.u[0] = (uint32_t)s_loopRec;
                        r.u[1] = (uint32_t)s_loopDom;
                        break;
                    case OP_LOOP:
                        loopTestNow(r.u[0], r.u[1]);
                        r.status = 1;
                        break;
                    case OP_TXPULLS: {
                        int fl = -1, up = -1, down = -1;
                        r.status = txPullsNow(fl, up, down) ? 1 : 0;
                        r.u[0] = (uint32_t)fl; r.u[1] = (uint32_t)up; r.u[2] = (uint32_t)down;
                        break;
                    }
                    case OP_PINTEST: {
                        bool rxOk = false, txOk = false;
                        r.status = pinTestNow(rxOk, txOk) ? 1 : 0;
                        r.u[0] = rxOk ? 1u : 0u; r.u[1] = txOk ? 1u : 0u;
                        break;
                    }
                    case OP_CRXPULLS: {
                        int raw, up, down, ctx;
                        crxPullsNow(raw, up, down, ctx);
                        r.status = 1;
                        r.u[0] = (uint32_t)raw; r.u[1] = (uint32_t)up;
                        r.u[2] = (uint32_t)down; r.u[3] = (uint32_t)ctx;
                        break;
                    }
                    default:
                        r.status = -1;
                        break;
                }
                return r;
            }

            void serveRequests()
            {
                RR::Ticket t;
                while (s_rrQ && xQueueReceive(s_rrQ, &t, 0) == pdTRUE) {
                    RR::Args a;
                    if (!s_rr.take(t, a)) continue;          // the waiter gave up
                    s_rr.complete(t, run(a));
                }
            }

            void sample(uint32_t now)
            {
                TwaiBus& bus = TwaiBus::instance();
                CanStatus c;
                c.installed = bus.isLive();
                c.mode = (uint8_t)bus.mode();
                if (c.installed) {
                    uint32_t a = 0;
                    if (twai_read_alerts(&a, 0) == ESP_OK) s_alertsNow |= a;
                    s_infoValid = twai_get_status_info(&s_info) == ESP_OK;
                } else {
                    s_infoValid = false;
                }
                if ((uint32_t)(now - s_bucketAtMs) >= ALERT_BUCKET_MS) {
                    s_alertsPrev = s_alertsNow;
                    s_alertsNow = 0;
                    s_bucketAtMs = now;
                }
                ++s_seq;
                if (s_infoValid) {
                    c.state    = (uint8_t)s_info.state;
                    c.tec      = s_info.tx_error_counter;
                    c.rec      = s_info.rx_error_counter;
                    c.txFailed = s_info.tx_failed_count;
                    c.rxMissed = s_info.rx_missed_count;
                    c.busErr   = s_info.bus_error_count;
                    c.arbLost  = s_info.arb_lost_count;
                    c.msgsToTx = s_info.msgs_to_tx;
                    c.msgsToRx = s_info.msgs_to_rx;
                }
                c.alerts     = s_alertsNow | s_alertsPrev;
                c.twaiTxOk   = bus.txOk();
                c.twaiTxFail = bus.txFail();
                c.twaiTxBusy = bus.txBusy();
                c.twaiRx     = bus.rxCount();
                c.installs   = bus.installs();
                c.atMs       = now;
                s_status.publish(c);
            }

            bool postTicket(const RR::Ticket& t, void*)
            {
                return s_rrQ && xQueueSend(s_rrQ, &t, 0) == pdTRUE;
            }

            bool call(const RR::Args& a, RR::Result& r)
            {
                return s_rr.call(a, r, postTicket, nullptr) == RR::Channel::Call::Ok;
            }
        }

        void begin(const Setup& s)
        {
            s_s = s;
            s_listenOnly = s.listenOnly;
            s_rrQ = xQueueCreateStatic(RR_DEPTH, sizeof(RR::Ticket), s_rrStore, &s_rrBuf);
            s_rr.begin();
            // A first sample now, so a page opened before the first pass reads
            // something real.
            sample(millis());
            s_sampleAtMs = millis();
        }

        void service(uint32_t nowMs)
        {
            serveRequests();
            if ((uint32_t)(nowMs - s_sampleAtMs) >= SAMPLE_MS) {
                s_sampleAtMs = nowMs;
                sample(nowMs);
            }
        }

        const twai_status_info_t& info() { return s_info; }
        bool infoValid() { return s_infoValid; }
        uint32_t seq() { return s_seq; }

        CanStatus status() { return s_status.read(); }
        bool listenOnly() { return s_listenOnly; }
        void lastSelfTest(int& recessive, int& dominant)
        {
            recessive = s_loopRec;
            dominant  = s_loopDom;
        }

        bool setMode(twai_mode_t mode, bool& ok)
        {
            RR::Args a; a.op = OP_MODE; a.u[0] = (uint32_t)mode;
            RR::Result r;
            if (!call(a, r)) return false;
            ok = r.status == 1;
            return true;
        }

        bool setListen(bool listenOnly, bool& ok)
        {
            RR::Args a; a.op = OP_LISTEN; a.u[0] = listenOnly ? 1u : 0u;
            RR::Result r;
            if (!call(a, r)) return false;
            ok = r.status == 1;
            return true;
        }

        bool setTiming(const TwaiBus::Timing& t, bool& ok, uint16_t& sp, uint32_t& bitrate)
        {
            RR::Args a; a.op = OP_TIMING;
            a.u[0] = t.brp; a.u[1] = t.tseg1; a.u[2] = t.tseg2; a.u[3] = t.sjw;
            RR::Result r;
            if (!call(a, r)) return false;
            ok = r.status == 1;
            sp = (uint16_t)r.u[0];
            bitrate = r.u[1];
            return true;
        }

        bool selfTest(bool& ok, int& recessive, int& dominant)
        {
            RR::Args a; a.op = OP_SELFTEST;
            RR::Result r;
            if (!call(a, r)) return false;
            ok = r.status == 1;
            recessive = (int)r.u[0];
            dominant  = (int)r.u[1];
            return true;
        }

        bool loopTest(uint32_t& cycles, uint32_t& followed)
        {
            RR::Args a; a.op = OP_LOOP;
            RR::Result r;
            if (!call(a, r)) return false;
            cycles = r.u[0];
            followed = r.u[1];
            return true;
        }

        bool txPulls(bool& ok, int& floating, int& pullup, int& pulldown)
        {
            RR::Args a; a.op = OP_TXPULLS;
            RR::Result r;
            if (!call(a, r)) return false;
            ok = r.status == 1;
            floating = (int)r.u[0];
            pullup   = (int)r.u[1];
            pulldown = (int)r.u[2];
            return true;
        }

        bool pinTest(bool& ok, bool& rxPadOk, bool& txPadOk)
        {
            RR::Args a; a.op = OP_PINTEST;
            RR::Result r;
            if (!call(a, r)) return false;
            ok = r.status == 1;
            rxPadOk = r.u[0] != 0;
            txPadOk = r.u[1] != 0;
            return true;
        }

        bool crxPulls(int& raw, int& up, int& down, int& ctxRaw)
        {
            RR::Args a; a.op = OP_CRXPULLS;
            RR::Result r;
            if (!call(a, r)) return false;
            raw    = (int)r.u[0];
            up     = (int)r.u[1];
            down   = (int)r.u[2];
            ctxRaw = (int)r.u[3];
            return true;
        }
    }
}
#endif
