#pragma once
#include <stdint.h>
#include <atomic>

// Who else is on the panel bus, from what it sends: listening only, never a
// frame of its own.
//
//   * A foreign bus master: an OEM radio already drives the handshake, its
//     sync on 0x3AF (Carminat) or 0x3DF (UpdateList). Those are the master's
//     ids (AffaDisplay's kIdSync); the panel never sends them, and the TWAI
//     controller does not receive our own frames. MASTER_MIN syncs within
//     WINDOW_MS make it a master; it stays one until REARM_MS pass with no
//     sync at all (the radio was removed or switched off). An OEM radio
//     syncs about once a second (our own AFFA_SYNC_INTERVAL_MS is 1000).
//   * A lone cluster: 0x3CF DLC 1 `69` pings and never the panel's `61 11`
//     request on 0x3CF. There is no AFFA panel here to register with.
//
// mayTransmit(): our stack keeps its TX gate shut until it has listened for
// LISTEN_MS (counted from begin(), which the panel task calls at its first
// poll(): before that nothing is read, so nothing was heard) with no foreign
// sync in the last LISTEN_MS. So ONE sync is enough to keep us quiet, and an
// OEM radio is always heard before we send a sync of our own.
//
// onRx() runs inside poll() (the link's raw tap, the panel task); the
// verdicts are read there and by httpd: relaxed atomics. Its nowMs is when
// poll() drained the frame, not when it arrived: syncs that queued up before
// the first poll() read as one burst. That errs on the quiet side (it can
// only make a master sooner, never miss one).
// Pure (no Arduino): host-tested in test/test_panelwatch.
namespace MegaOpen
{
    class PanelBusWatch {
    public:
        static constexpr uint32_t LISTEN_MS   = 3000;
        static constexpr uint32_t WINDOW_MS   = 3000;
        static constexpr uint32_t MASTER_MIN  = 2;
        static constexpr uint32_t REARM_MS    = 30000;
        static constexpr uint32_t CLUSTER_MIN = 3;
        static constexpr uint32_t ID_SYNC_CARMINAT   = 0x3AF;
        static constexpr uint32_t ID_SYNC_UPDATELIST = 0x3DF;
        static constexpr uint32_t ID_SYNC_REPLY      = 0x3CF;

        // The panel task's first poll(): the listen window starts here. Later
        // calls change nothing.
        void begin(uint32_t nowMs)
        {
            if (_started.load(std::memory_order_relaxed)) return;
            _t0.store(nowMs, std::memory_order_relaxed);
            _started.store(true, std::memory_order_relaxed);
        }
        bool started() const { return _started.load(std::memory_order_relaxed); }

        // A frame the controller received (never one we sent), at nowMs.
        void onRx(uint32_t id, uint8_t len, const uint8_t* d, uint32_t nowMs)
        {
            if (id == ID_SYNC_CARMINAT || id == ID_SYNC_UPDATELIST) {
                const uint32_t gap = sinceSync(nowMs);
                // Quiet for REARM_MS: whatever drove the bus is gone; this
                // sync starts a fresh count rather than re-latching at once.
                if (gap >= REARM_MS) _latched.store(false, std::memory_order_relaxed);
                const bool recent = gap < WINDOW_MS;
                const uint32_t burst = recent ? _burst.load(std::memory_order_relaxed) + 1 : 1;
                _burst.store(burst, std::memory_order_relaxed);
                _lastSync.store(nowMs, std::memory_order_relaxed);
                _heard.store(true, std::memory_order_relaxed);
                _master.fetch_add(1, std::memory_order_relaxed);
                _masterId.store(id, std::memory_order_relaxed);
                if (burst >= MASTER_MIN) _latched.store(true, std::memory_order_relaxed);
            } else if (id == ID_SYNC_REPLY && d) {
                if (len == 1 && d[0] == 0x69)                      _cluster.fetch_add(1, std::memory_order_relaxed);
                else if (len >= 2 && d[0] == 0x61 && d[1] == 0x11) _panel.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // Another master drives this bus: MASTER_MIN syncs within WINDOW_MS,
        // and a sync within the last REARM_MS.
        bool foreignMaster(uint32_t nowMs) const
        {
            return _latched.load(std::memory_order_relaxed) && sinceSync(nowMs) < REARM_MS;
        }
        // Which sync id the last foreign master used (0 = none seen).
        uint32_t masterId() const { return _masterId.load(std::memory_order_relaxed); }
        bool clusterOnly() const  { return clusterPings() >= CLUSTER_MIN && panelRequests() == 0; }

        // Our panel stack may transmit: begin() was called, LISTEN_MS have
        // passed since, no foreign sync in the last LISTEN_MS, and no master.
        bool mayTransmit(uint32_t nowMs) const
        {
            if (!started()) return false;
            if ((uint32_t)(nowMs - _t0.load(std::memory_order_relaxed)) < LISTEN_MS) return false;
            if (_heard.load(std::memory_order_relaxed) && sinceSync(nowMs) < LISTEN_MS) return false;
            return !foreignMaster(nowMs);
        }

        uint32_t masters() const       { return _master.load(std::memory_order_relaxed); }
        uint32_t clusterPings() const  { return _cluster.load(std::memory_order_relaxed); }
        uint32_t panelRequests() const { return _panel.load(std::memory_order_relaxed); }

    private:
        uint32_t sinceSync(uint32_t nowMs) const
        {
            if (!_heard.load(std::memory_order_relaxed)) return UINT32_MAX;
            return (uint32_t)(nowMs - _lastSync.load(std::memory_order_relaxed));
        }

        std::atomic<bool>     _started{false}, _heard{false}, _latched{false};
        std::atomic<uint32_t> _t0{0}, _lastSync{0}, _burst{0};
        std::atomic<uint32_t> _master{0}, _masterId{0}, _cluster{0}, _panel{0};
    };
}
