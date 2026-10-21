#pragma once
#include <stdint.h>
#include <atomic>

// Who else is on the panel bus, from what it sends: listening only, never a
// frame of its own.
//
//   * A foreign bus master: an OEM radio already drives the handshake, its
//     sync on 0x3AF (Carminat) or 0x3DF (UpdateList). Those are the master's
//     ids (AffaDisplay's kIdSync); the panel never sends them, and the TWAI
//     controller does not receive our own frames. Two of them latch it until
//     the next boot: the panel stack must then stay silent (passive and
//     listen-only), or two masters fight over one panel.
//   * A lone cluster: 0x3CF DLC 1 `69` pings and never the panel's `61 11`
//     request on 0x3CF. There is no AFFA panel here to register with.
//
// mayTransmit(): our stack keeps its TX gate shut for LISTEN_MS after the
// link comes up, so an OEM radio is heard before we ever send a sync, and
// shut for good once a foreign master was seen.
//
// onRx() runs inside poll() (the link's raw tap, the panel task); the
// verdicts are read there and by httpd: relaxed atomics.
// Pure (no Arduino): host-tested in test/test_panelwatch.
namespace MegaOpen
{
    class PanelBusWatch {
    public:
        static constexpr uint32_t LISTEN_MS   = 1000;
        static constexpr uint32_t MASTER_MIN  = 2;
        static constexpr uint32_t CLUSTER_MIN = 3;
        static constexpr uint32_t ID_SYNC_CARMINAT   = 0x3AF;
        static constexpr uint32_t ID_SYNC_UPDATELIST = 0x3DF;
        static constexpr uint32_t ID_SYNC_REPLY      = 0x3CF;

        // When the link came up: the listen window starts here.
        void begin(uint32_t nowMs) { _t0 = nowMs; _started.store(true, std::memory_order_relaxed); }

        // A frame the controller received (never one we sent).
        void onRx(uint32_t id, uint8_t len, const uint8_t* d)
        {
            if (id == ID_SYNC_CARMINAT || id == ID_SYNC_UPDATELIST) {
                _master.fetch_add(1, std::memory_order_relaxed);
                _masterId.store(id, std::memory_order_relaxed);
            } else if (id == ID_SYNC_REPLY && d) {
                if (len == 1 && d[0] == 0x69)                         _cluster.fetch_add(1, std::memory_order_relaxed);
                else if (len >= 2 && d[0] == 0x61 && d[1] == 0x11)    _panel.fetch_add(1, std::memory_order_relaxed);
            }
        }

        bool foreignMaster() const { return masters() >= MASTER_MIN; }
        // Which sync id the foreign master used (0 = none seen).
        uint32_t masterId() const  { return _masterId.load(std::memory_order_relaxed); }
        bool clusterOnly() const   { return clusterPings() >= CLUSTER_MIN && panelRequests() == 0; }

        // Our panel stack may transmit: past the listen window, and no other
        // master on the bus. False before begin().
        bool mayTransmit(uint32_t nowMs) const
        {
            return _started.load(std::memory_order_relaxed) &&
                   (uint32_t)(nowMs - _t0) >= LISTEN_MS && !foreignMaster();
        }

        uint32_t masters() const       { return _master.load(std::memory_order_relaxed); }
        uint32_t clusterPings() const  { return _cluster.load(std::memory_order_relaxed); }
        uint32_t panelRequests() const { return _panel.load(std::memory_order_relaxed); }

    private:
        uint32_t _t0 = 0;
        std::atomic<bool>     _started{false};
        std::atomic<uint32_t> _master{0}, _masterId{0}, _cluster{0}, _panel{0};
    };
}
