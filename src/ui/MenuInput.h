#pragma once
#include <stdint.h>
#include "../keys/KeyAction.h"
#include "MenuEngine.h"

// Which keys the panel menu takes, decided on the keys task ahead of the
// macro filter. A verdict is a few compares and one bit: no allocation, no
// lock, nothing that waits.
//
//   menu closed   Action::Menu opens it (the right knob's hold by default).
//                 Everything else passes.
//   menu open     Menu closes it. The scroll keys move (EncCw/EncCcw: the
//                 knob and the wheel; SrcNext/SrcPrev: the stalk), Ok and Load
//                 select. Volume, mute, phone and the rest pass, so the
//                 volume popup still comes up over the menu.
//   panel off     A press of either knob (anything from the knobs but a turn)
//                 only switches the panel on. Turns still change the volume.
//
// A swallowed press takes its Long, Repeat and Release with it, and only
// those: a key already down when the menu opened is released normally.
namespace MenuUi
{
    enum class Mode : uint8_t { Closed, Open, PanelOff };

    struct Verdict {
        enum class Do : uint8_t { Pass, Swallow, Open, Command, PowerOn };
        Do  what = Do::Pass;
        Cmd cmd = Cmd::Next;       // Do::Command
        bool consumed() const { return what != Do::Pass; }
    };

    class InputFilter {
    public:
        static constexpr uint8_t KNOB_SRC = 4;    // Keys::Event::srcId of the knobs

        Verdict on(const Keys::Event& e, Mode m);

    private:
        bool takeHeld(Keys::Action a);
        void hold(Keys::Action a);
        uint32_t _held = 0;                       // one bit per swallowed action
    };
}
