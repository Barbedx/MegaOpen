#include "CanBench.h"
#if FEATURE_DISPLAY_CAN || FEATURE_VEHICLE_CAN
#include "../utils/Snapshot.h"

namespace Diag
{
    namespace CanBench
    {
        namespace
        {
            constexpr uint32_t SAMPLE_MS = 100;
            constexpr uint32_t ALERT_BUCKET_MS = 1000;

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
        }

        void begin(const Setup& s)
        {
            s_listenOnly = s.listenOnly;
            // A first sample now, so a page opened before the first pass reads
            // something real.
            sample(millis());
            s_sampleAtMs = millis();
        }

        void service(uint32_t nowMs)
        {
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
    }
}
#endif
