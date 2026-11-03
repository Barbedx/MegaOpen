#include "RadioCode.h"
#include <string.h>

namespace MegaOpen
{
    void RadioCode::begin(SendFn send, void* ctx)
    {
        _send = send;
        _ctx  = ctx;
    }

    void RadioCode::setCode(const char* code)
    {
        bool ok = code != nullptr;
        if (ok) {
            size_t n = 0;
            for (; code[n]; ++n) if (code[n] < '0' || code[n] > '9') { ok = false; break; }
            ok = ok && (n == 0 || n == 4);
        }
        if (ok) memcpy(_code, code, 5); else _code[0] = 0;
        if (!enabled()) { settle(State::Off); return; }
        if (_v.state == State::Off) settle(State::Idle);
    }

    // "  CODE " after the 0x21 PCI: 21 20 20 43 4F 44 45 20 [CAP].
    bool RadioCode::promptFrame(const uint8_t* d, uint8_t len)
    {
        static const uint8_t WORD[7] = {0x20, 0x20, 'C', 'O', 'D', 'E', 0x20};
        return len >= 8 && d[0] == 0x21 && memcmp(d + 1, WORD, 7) == 0;
    }

    // The entry field: "  " then four places, the one under the cursor shown
    // as 0xB0..0xB3 (the cursor index), the others digits: 21 20 20 B0 30 30 30 20 [CAP].
    bool RadioCode::fieldFrame(const uint8_t* d, uint8_t len)
    {
        if (len < 8 || d[0] != 0x21 || d[1] != 0x20 || d[2] != 0x20) return false;
        bool cursor = false;
        for (uint8_t i = 3; i < 7; ++i) {
            const uint8_t c = d[i];
            if (c >= 0xB0 && c <= 0xB3) cursor = true;
            else if (c < '0' || c > '9') return false;
        }
        return cursor;
    }

    void RadioCode::onRx(uint32_t id, const uint8_t* d, uint8_t len, uint32_t nowMs)
    {
        if (!enabled() || id != ID_TEXT || !d) return;
        if (!promptFrame(d, len) && !fieldFrame(d, len)) return;
        _v.promptAt = nowMs ? nowMs : 1;
        switch (_v.state) {
            case State::Idle: {
                const bool cool = _lastAttemptAt && (uint32_t)(nowMs - _lastAttemptAt) < COOLDOWN_MS;
                if (cool) {
                    if (!_skipSaid) { _skipSaid = true; _event = Event::Skipped; }
                    return;
                }
                settle(State::Armed);
                _nextAt = nowMs + START_MS;
                _event  = Event::Prompt;
                return;
            }
            case State::Verdict:
                // The radio echoes the field after every key, the last one
                // included: only a prompt well after the last key is its
                // answer.
                if ((uint32_t)(nowMs - _lastKeyAt) > ECHO_MS) _promptAfterKeys = true;
                return;
            default:
                return;
        }
    }

    // The attempt, from the code: the wheel up d times for digit d (every
    // place starts at 0), Load to the next place, Load held after the last.
    void RadioCode::plan()
    {
        _nSteps = 0;
        for (uint8_t i = 0; i < 4; ++i) {
            const uint8_t d = (uint8_t)(_code[i] - '0');
            for (uint8_t k = 0; k < d; ++k) _steps[_nSteps++] = Step{KEY_ROLL_UP, false};
            _steps[_nSteps++] = Step{KEY_LOAD, i == 3};
        }
        _at = 0;
        _fails = 0;
    }

    void RadioCode::tick(uint32_t nowMs)
    {
        switch (_v.state) {
            case State::Armed:
                if ((int32_t)(nowMs - _nextAt) < 0) return;
                plan();
                ++_v.attempts;
                _v.sent = 0;
                _lastAttemptAt = nowMs ? nowMs : 1;
                _skipSaid = false;
                settle(State::Typing);
                _nextAt = nowMs;
                // fall through: the first key now
            case State::Typing: {
                if ((int32_t)(nowMs - _nextAt) < 0) return;
                const Step& s = _steps[_at];
                if (!_send || !_send(s.wire, s.hold, _ctx)) {
                    if (++_fails >= SEND_TRIES) {
                        _event = Event::NoBus;
                        settle(State::Idle);
                        return;
                    }
                    _nextAt = nowMs + KEY_GAP_MS;
                    return;
                }
                _fails = 0;
                ++_v.sent;
                _lastKeyAt = nowMs;
                _nextAt = nowMs + KEY_GAP_MS;
                if (++_at >= _nSteps) {
                    _promptAfterKeys = false;
                    _nextAt = nowMs + RESULT_MS;
                    _event = Event::Typed;
                    settle(State::Verdict);
                }
                return;
            }
            case State::Verdict:
                if (_promptAfterKeys) {
                    ++_v.rejected;
                    _event = Event::Rejected;
                    settle(State::Idle);
                    return;
                }
                if ((int32_t)(nowMs - _nextAt) < 0) return;
                ++_v.accepted;
                _event = Event::Accepted;
                settle(State::Idle);
                return;
            default:
                return;
        }
    }
}
