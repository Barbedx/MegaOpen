#pragma once
// Conn handle -> peer state for the two simultaneous links (iPhone AMS/ANCS/CTS,
// head unit HID). No String/heap: touched from the NimBLE host task.

#include <NimBLEDevice.h>
#include <string.h>

namespace Ble
{
    enum class PeerRole : uint8_t
    {
        Unknown, // just connected, not yet classified
        Ams,     // iPhone — we hold a GATT client onto it
        Hid,     // head unit — it subscribed to our HID input report
        NotAms,  // probed for AMS, does not have it; never probe again this link
    };

    inline const char *roleName(PeerRole r)
    {
        switch (r)
        {
        case PeerRole::Ams:    return "ams";
        case PeerRole::Hid:    return "hid";
        case PeerRole::NotAms: return "not-ams";
        default:               return "unknown";
        }
    }

    struct Peer
    {
        uint16_t handle      = BLE_HS_CONN_HANDLE_NONE;
        PeerRole role        = PeerRole::Unknown;
        bool     encrypted   = false;
        bool     clientBound = false; // true for AT MOST one peer (getClient is a singleton)
        bool     paramsRelaxed = false;
        // Separate from paramsRelaxed (30-50ms on encrypt): a keyboard host then
        // gets a second, wider 100-125ms step.
        bool     hidParamsApplied = false;
        // Subscribed to our HID report. An iPhone bonded as keyboard host does this
        // too; AMS presence decides (classifyAsHid(), SETUP_DEADLINE_MS).
        bool     hidSubscribed = false;
        uint32_t connectedMs = 0;
        uint32_t securedMs   = 0; // when the link became encrypted, 0 while not
        uint8_t  secureTries = 0; // async pairing attempts initiated
        char     addr[18]    = {0}; // over-the-air address (may be a rotating RPA)
        char     idAddr[18]  = {0}; // identity address — stable, but only once bonded

        bool inUse() const { return handle != BLE_HS_CONN_HANDLE_NONE; }
    };

    // kMax must match CONFIG_BT_NIMBLE_MAX_CONNECTIONS.
    class PeerTable
    {
    public:
        static constexpr uint8_t kMax = 2;

        Peer *add(uint16_t h, const char *addr, const char *idAddr)
        {
            if (Peer *existing = byHandle(h))
                return existing;
            for (uint8_t i = 0; i < kMax; i++)
            {
                if (m_peers[i].inUse())
                    continue;
                m_peers[i] = Peer{};
                m_peers[i].handle = h;
                copyAddr(m_peers[i].addr, addr);
                copyAddr(m_peers[i].idAddr, idAddr);
                return &m_peers[i];
            }
            return nullptr; // table full — NimBLE let in more than kMax
        }

        Peer *byHandle(uint16_t h)
        {
            if (h == BLE_HS_CONN_HANDLE_NONE)
                return nullptr;
            for (uint8_t i = 0; i < kMax; i++)
                if (m_peers[i].inUse() && m_peers[i].handle == h)
                    return &m_peers[i];
            return nullptr;
        }

        Peer *byRole(PeerRole r)
        {
            for (uint8_t i = 0; i < kMax; i++)
                if (m_peers[i].inUse() && m_peers[i].role == r)
                    return &m_peers[i];
            return nullptr;
        }

        Peer *at(uint8_t i) { return (i < kMax && m_peers[i].inUse()) ? &m_peers[i] : nullptr; }

        void remove(uint16_t h)
        {
            if (Peer *p = byHandle(h))
                *p = Peer{};
        }

        uint8_t count() const
        {
            uint8_t n = 0;
            for (uint8_t i = 0; i < kMax; i++)
                if (m_peers[i].inUse())
                    n++;
            return n;
        }

        // Exactly one peer may ever hold the server's single NimBLEClient.
        bool anyClientBound()
        {
            for (uint8_t i = 0; i < kMax; i++)
                if (m_peers[i].inUse() && m_peers[i].clientBound)
                    return true;
            return false;
        }

    private:
        static void copyAddr(char *dst, const char *src)
        {
            if (!src)
            {
                dst[0] = 0;
                return;
            }
            strncpy(dst, src, 17);
            dst[17] = 0;
        }

        Peer m_peers[kMax];
    };

    inline bool addrEq(const char *a, const char *b)
    {
        return a && b && a[0] && b[0] && strcasecmp(a, b) == 0;
    }
} // namespace Ble
