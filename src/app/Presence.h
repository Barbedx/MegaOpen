#pragma once
#include <stdint.h>
#include <atomic>

// Someone at the board: the routes that can take the board over (an OTA image,
// a GitHub install, a factory reset, a settings file applied) are allowed only
// within WINDOW_MS of a physical act there: the panel menu's "Allow update",
// or the board's BOOT button. The WiFi password guards the board's own AP, but
// on a home network the board shares a LAN with every other device; this
// keeps a compromised one from flashing it.
//
// press() from the panel task (the menu) or net_svc (the button); allowed()
// from httpd: relaxed atomics. Pure (no Arduino): host-tested in test/test_ota.
namespace App
{
    class Presence {
    public:
        static constexpr uint32_t WINDOW_MS = 120000;

        void press(uint32_t nowMs)
        {
            _at.store(nowMs, std::memory_order_relaxed);
            _pressed.store(true, std::memory_order_relaxed);
            _presses.fetch_add(1, std::memory_order_relaxed);
        }

        // Within WINDOW_MS of the last press.
        bool allowed(uint32_t nowMs) const { return remainingMs(nowMs) > 0; }

        // How long the window stays open, 0 = shut (never pressed, or past it).
        uint32_t remainingMs(uint32_t nowMs) const
        {
            if (!_pressed.load(std::memory_order_relaxed)) return 0;
            const uint32_t since = nowMs - _at.load(std::memory_order_relaxed);
            return since < WINDOW_MS ? WINDOW_MS - since : 0;
        }

        uint32_t presses() const { return _presses.load(std::memory_order_relaxed); }

    private:
        std::atomic<bool>     _pressed{false};
        std::atomic<uint32_t> _at{0}, _presses{0};
    };

    // The board's one window: the panel menu and the BOOT button press it,
    // the web routes ask it.
    inline Presence& presence()
    {
        static Presence p;
        return p;
    }
}
