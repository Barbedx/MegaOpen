#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// The board's own access point password, per board (security review P1).
//
// The old default, "megaopen", is public, and every release image carried it,
// so anyone in range of a parked car could join and flash the board. Now:
//
//   * A fresh or factory-reset board makes up its own password (LEN letters
//     and digits, no look-alikes) and shows it on the car's display at that
//     boot; the panel menu's "WiFi pass" row shows it again. It never goes
//     to the web or the web log: being in the car is the proof.
//   * A board still on the public default keeps it (no lock-out) and gets a
//     candidate, shown on the panel at every boot until the owner adopts it
//     from the menu, or sets an own password on the web.
//   * A dev build may preset one in src/secrets.h (MO_AP_PASS); the public
//     default there counts as none.
//   * The web refuses the public default as a new password.
//
// Pure: host-tested in test_settings.
namespace ApPass
{
    inline constexpr const char* PUBLIC_DEFAULT = "megaopen";
    inline constexpr uint8_t     LEN = 10;
    // No 0/o, 1/l/i: read off a car's display and typed into a phone.
    inline constexpr char ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789";
    inline constexpr uint8_t ALPHABET_N = sizeof(ALPHABET) - 1;

    // An AP password the board may use: WPA2's 8..63, not the public default.
    inline bool acceptable(const char* p)
    {
        if (!p) return false;
        const size_t n = strlen(p);
        return n >= 8 && n <= 63 && strcmp(p, PUBLIC_DEFAULT) != 0;
    }

    // LEN characters and a NUL into `out`, from `rnd()` (uint32_t; esp_random
    // on the board). Rejection sampling, so every character is equally likely.
    template <class Rnd>
    void generate(char* out, Rnd&& rnd)
    {
        const uint32_t limit = UINT32_MAX - UINT32_MAX % ALPHABET_N;
        for (uint8_t i = 0; i < LEN; ++i) {
            uint32_t r;
            do { r = rnd(); } while (r >= limit);
            out[i] = ALPHABET[r % ALPHABET_N];
        }
        out[LEN] = '\0';
    }

    enum class State : uint8_t {
        Own,        // an own password (stored, or a dev preset): nothing to show
        Fresh,      // made up at this boot: show it on the panel
        Pending,    // still the public default: a candidate waits to be adopted
    };

    // What Config::begin() does with what NVS holds. `stored` is the NVS
    // value or nullptr when there is none; `preset` is MO_AP_PASS.
    inline State decide(const char* stored, const char* preset)
    {
        if (stored && *stored) return strcmp(stored, PUBLIC_DEFAULT) == 0 ? State::Pending : State::Own;
        return acceptable(preset) ? State::Own : State::Fresh;
    }
}
