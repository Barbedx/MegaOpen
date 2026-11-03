#pragma once
#include <stddef.h>
#include <stdint.h>

// The stock radio's code, typed for the owner.
//
// With FuncsReg off the OEM radio owns the panel and MegaOpen only listens
// and injects. After the battery was off, the radio asks for its four-digit
// code on the panel: the text "  CODE  " on 0x151, then the entry field
// "  <cursor>000 " with 0xB0 for the blinking cursor on digit 0 (0xB1.. on
// the next ones). The code is typed with the stalk: the wheel up adds one to
// the digit under the cursor, the bottom button (Load) moves to the next
// digit, and Load held confirms (MeganeCAN, notes/AFFA3_SCREENS.md, from the
// car's captures). This types it from the setting `radioCode` by sending the
// panel's own key frames to the radio (pressKey(..., Wire) on the panel
// task), one key every KEY_GAP_MS.
//
// One attempt per prompt, never two within COOLDOWN_MS: a wrong code makes
// the radio wait longer each time, so a wrong setting must not hammer it.
// The verdict is the screen's: the prompt gone RESULT_MS after the last key
// is accepted; the prompt or the field back is rejected (logged: the owner
// checks the setting).
//
// Pure (no Arduino): the host test feeds it the captured frames and reads
// the keys it would send. Runs on the panel task: onRx() from the frame tap,
// tick() from the loop.
namespace MegaOpen
{
    class RadioCode {
    public:
        static constexpr uint32_t ID_TEXT     = 0x151;    // the radio's text to the panel
        static constexpr uint16_t KEY_ROLL_UP = 0x0101;   // affa::Key::RollUp
        static constexpr uint16_t KEY_LOAD    = 0x0000;   // affa::Key::Load
        static constexpr uint32_t START_MS    = 800;      // the prompt seen: let the screen settle
        static constexpr uint32_t KEY_GAP_MS  = 160;      // between two keys
        static constexpr uint32_t ECHO_MS     = 500;      // the radio's echo of a key comes within this
        static constexpr uint32_t RESULT_MS   = 3000;     // after the last key: the screen's verdict
        static constexpr uint32_t COOLDOWN_MS = 60000;    // between two attempts
        static constexpr uint8_t  SEND_TRIES  = 10;       // a key the bus refused, before giving up

        enum class State : uint8_t { Off, Idle, Armed, Typing, Verdict };
        // For the log, once each: the caller reads and clears.
        enum class Event : uint8_t { None, Prompt, Typed, Accepted, Rejected, Skipped, NoBus };

        // The key frame onto the bus: wire code, hold edge. false: not sent.
        using SendFn = bool (*)(uint16_t wire, bool hold, void* ctx);
        void begin(SendFn send, void* ctx);
        // Four digits, or "" (off). Anything else is off too.
        void setCode(const char* code);
        bool enabled() const { return _code[0] != 0; }
        const char* code() const { return _code; }

        // Received frames on the panel bus (never our own). Cheap when off.
        void onRx(uint32_t id, const uint8_t* d, uint8_t len, uint32_t nowMs);
        // Every pass: the next key when due, the verdict when it is time.
        void tick(uint32_t nowMs);

        struct View {
            State    state = State::Off;
            uint32_t attempts = 0;      // this boot
            uint32_t promptAt = 0;      // of the last prompt seen, 0 none
            uint32_t sent = 0;          // keys sent in the current or last attempt
            uint32_t accepted = 0, rejected = 0;
        };
        const View& view() const { return _v; }
        Event takeEvent() { const Event e = _event; _event = Event::None; return e; }

        // The two frames of the prompt, as the panel receives them: a
        // consecutive ISO-TP frame (0x21) with "  CODE " or "  <B0..B3>...".
        static bool promptFrame(const uint8_t* d, uint8_t len);
        static bool fieldFrame(const uint8_t* d, uint8_t len);

    private:
        void plan();
        void settle(State s) { _v.state = s; }

        SendFn   _send = nullptr;
        void*    _ctx  = nullptr;
        char     _code[5] = "";
        View     _v;
        Event    _event = Event::None;
        // The attempt: at most 9 wheel clicks a digit, then Load for each.
        struct Step { uint16_t wire; bool hold; };
        Step     _steps[4 * 9 + 4];
        uint8_t  _nSteps = 0, _at = 0, _fails = 0;
        uint32_t _nextAt = 0;
        uint32_t _lastAttemptAt = 0;   // 0: none yet
        uint32_t _lastKeyAt = 0;
        bool     _promptAfterKeys = false;
        bool     _skipSaid = false;
    };
}
