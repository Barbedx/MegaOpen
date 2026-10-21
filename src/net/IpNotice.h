#pragma once
#include <stdint.h>

// When to put the board's address on the panel (docs/BACKLOG.md): once per
// connection, not on every flap. Portable: the host tests drive it with
// numbers; main.cpp feeds it the link from net_svc every pass and queues the
// notification when it says so.
//
// A link that comes back within FLAP_MS with the same mode and address is the
// same connection: nothing new to say. A new address, a mode change (the AP
// after the home network was lost, the home network after the AP) or a link
// down for FLAP_MS or longer is announced, and so is the first link of a boot.
class IpNotice {
public:
    static constexpr uint32_t FLAP_MS = 30000;

    enum class Mode : uint8_t { None, Sta, Ap };

    // Every pass: the link now (None while down or still joining) and its
    // IPv4 address. True on the pass the notice is due.
    bool tick(uint32_t nowMs, Mode mode, uint32_t ip);

private:
    Mode     _shownMode = Mode::None;   // the last announced link
    uint32_t _shownIp   = 0;
    bool     _up        = false;        // up at the last pass
    uint32_t _downAt    = 0;            // when it went down; valid while !_up
    bool     _everDown  = false;        // _downAt has been set
};
