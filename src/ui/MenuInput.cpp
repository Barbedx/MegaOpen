#include "MenuInput.h"

namespace MenuUi
{
    using Keys::Action;
    using Keys::State;

    static_assert((uint8_t)Action::_Count <= 32, "InputFilter::_held is one bit per action");

    void InputFilter::hold(Action a) { _held |= 1u << (uint8_t)a; }

    bool InputFilter::takeHeld(Action a)
    {
        const uint32_t bit = 1u << (uint8_t)a;
        const bool was = _held & bit;
        _held &= ~bit;
        return was;
    }

    Verdict InputFilter::on(const Keys::Event& e, Mode m)
    {
        Verdict v;
        const Action a = e.action;
        if (a == Action::None || (uint8_t)a >= (uint8_t)Action::_Count) return v;

        // The tail of a press this filter swallowed goes the same way.
        if (e.state != State::Press) {
            const uint32_t bit = 1u << (uint8_t)a;
            if (!(_held & bit)) return v;
            if (e.state == State::Release) takeHeld(a);
            // A held scroll key keeps scrolling the menu.
            if (e.state == State::Repeat && m == Mode::Open) {
                if (a == Action::EncCw || a == Action::SrcNext) { v.what = Verdict::Do::Command; v.cmd = Cmd::Next; return v; }
                if (a == Action::EncCcw || a == Action::SrcPrev) { v.what = Verdict::Do::Command; v.cmd = Cmd::Prev; return v; }
            }
            v.what = Verdict::Do::Swallow;
            return v;
        }

        // A press. Turns have no release, so they are never held.
        if (m == Mode::PanelOff) {
            const bool knobPress = e.srcId == KNOB_SRC && !Keys::isEncoder(a) && !Keys::isVolume(a);
            if (knobPress || a == Action::Menu) {
                hold(a);
                v.what = Verdict::Do::PowerOn;
            }
            return v;
        }

        if (a == Action::Menu) {
            hold(a);
            if (m == Mode::Closed) v.what = Verdict::Do::Open;
            else { v.what = Verdict::Do::Command; v.cmd = Cmd::Close; }
            return v;
        }

        if (m != Mode::Open) return v;

        switch (a) {
            case Action::EncCw:
            case Action::SrcNext: v.cmd = Cmd::Next; break;
            case Action::EncCcw:
            case Action::SrcPrev: v.cmd = Cmd::Prev; break;
            case Action::Ok:
            case Action::Load:    v.cmd = Cmd::Select; break;
            default:              return v;          // volume and the rest pass
        }
        if (!Keys::isEncoder(a)) hold(a);
        v.what = Verdict::Do::Command;
        return v;
    }
}
