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

            Snapshot<CanStatus> s_status;
            uint32_t            s_sampleAtMs = 0;

            // One writer (the owner); httpd reads the word.
            volatile bool s_listenOnly = true;

            const affa::TwaiLink* s_link = nullptr;

            void sample(uint32_t now)
            {
                CanStatus c;
                c.atMs = now;
                if (!s_link) { s_status.publish(c); return; }
                const affa::TwaiLink::Driver d = s_link->driver();
                c.installed  = d.installed;
                c.mode       = (uint8_t)(s_link->listenOnly() ? TWAI_MODE_LISTEN_ONLY
                                                              : TWAI_MODE_NORMAL);
                c.state      = d.state;
                c.tec        = d.txErr;
                c.rec        = d.rxErr;
                c.txFailed   = d.txFailed;
                c.rxMissed   = d.rxMissed;
                c.busErr     = d.busErr;
                c.arbLost    = d.arbLost;
                c.msgsToTx   = d.queuedTx;
                c.msgsToRx   = d.queuedRx;
                c.twaiTxOk   = d.txFrames;
                c.twaiTxFail = d.txRejected;
                c.twaiTxBusy = d.txBusy;
                c.twaiRx     = d.rxFrames;
                c.installs   = d.installs;
                s_status.publish(c);
            }
        }

        void begin(const Setup& s)
        {
            s_listenOnly = s.listenOnly;
            s_link = s.link;
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

        CanStatus status() { return s_status.read(); }
        bool listenOnly() { return s_listenOnly; }
    }
}
#endif
