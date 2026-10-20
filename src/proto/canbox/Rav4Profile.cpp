#include "Rav4Profile.h"

namespace Canbox
{
    uint8_t rav4CodeFor(Keys::Action a)
    {
        using A = Keys::Action;
        switch (a) {
            case A::VolUp:   return RAV4_VOL_UP;
            case A::VolDown: return RAV4_VOL_DOWN;
            // Mute has no code of its own on this profile — it is a long press of
            // SOURCE. (0x06 has been claimed elsewhere; the head unit's own code
            // maps it to -1 and drops it.) The sink sends the LONG state for it.
            case A::Mute:    return RAV4_SOURCE;
            case A::SrcNext: return RAV4_SOURCE;
            case A::SrcPrev: return RAV4_SOURCE;
            // The wheel is a rotary, so it gets rotary codes: one frame carrying a
            // detent count, no release.
            case A::EncCw:   return RAV4_KNOB_TUNE_UP;
            case A::EncCcw:  return RAV4_KNOB_TUNE_DOWN;
            case A::Ok:      return RAV4_ENTER;
            case A::Load:    return RAV4_ENTER;
            case A::Mode:    return RAV4_MODE;
            case A::Radio:   return RAV4_HOME;
            case A::Phone:   return RAV4_PICKUP;
            case A::Voice:   return RAV4_SPEECH;
            default:         return 0;
        }
    }
}
