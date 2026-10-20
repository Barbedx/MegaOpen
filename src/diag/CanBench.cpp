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
            enum Op : uint8_t { OP_LOOP = 1 };

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

            // One writer (the owner); httpd reads the word.
            volatile bool s_listenOnly = true;

            int8_t rxPin() { return Board::PIN_CAN_RX; }
            int8_t txPin() { return Board::PIN_CAN_TX; }

            // Give the pins back to the controller after the loop test bit-banged
            // them, in the role's mode — never promote a listen-only (vehicle)
            // node to an ACKing one.
            void twaiRestartInRole(int8_t tx, int8_t rx)
            {
                const twai_mode_t mode = (s_s.role == Bus::Role::Vehicle || s_listenOnly)
                                             ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL;
                TwaiBus::instance().begin((gpio_num_t)tx, (gpio_num_t)rx, 500000, mode);
                TwaiBus::instance().onReceive(s_s.rx, nullptr);
                TwaiBus::instance().onTransmit(s_s.txTap);
            }

            // Toggled loopback (/api/can/loop, the /wiggle page). A transceiver
            // echoes the bus on its receiver: CTX low drives the bus dominant and
            // CRX must read 0. On demand only: a dominant destroys any frame in
            // flight. Toggled, never held: a held dominant can disarm a
            // transceiver within ~1 ms. CTX low 50 us, sample CRX at 30 us, high
            // 50 us; 400 cycles in bursts with a yield. Restored in the role's mode.
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

            RR::Result run(const RR::Args& a)
            {
                RR::Result r;
                switch (a.op) {
                    case OP_LOOP:
                        loopTestNow(r.u[0], r.u[1]);
                        r.status = 1;
                        break;
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

        bool loopTest(uint32_t& cycles, uint32_t& followed)
        {
            RR::Args a; a.op = OP_LOOP;
            RR::Result r;
            if (!call(a, r)) return false;
            cycles = r.u[0];
            followed = r.u[1];
            return true;
        }

    }
}
#endif
