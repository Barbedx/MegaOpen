#pragma once
#include <stdint.h>

// May an automatic screen (the now-playing rows in any layout, the car
// screen, the track splash) be sent now, or is a popup on the glass?
//
// On this panel every screen covers the popup layer: a setText is drawn
// inside the popup's frame, a menu, info list or fullscreen replaces it. So
// while a popup is live nothing automatic is sent; it stays owed (dirty) and
// goes out on the first pass after the popup's deadline. A person's screen,
// a notification and the panel menu are not automatic and do not ask.
//
// Only the deadline counts, not whether the take-down was delivered: the
// take-down needs a registered panel, registration rides on a render, and a
// gate waiting for the take-down could hold every render for good.
//
// `popupUntilMs` is the ui task's deadline: millis() to take the popup down,
// 0 = none. Pure: host-tested in test_glass.
namespace MegaOpen
{
    inline bool popupHolds(uint32_t popupUntilMs, uint32_t nowMs)
    {
        return popupUntilMs && (int32_t)(nowMs - popupUntilMs) < 0;
    }
}
