#pragma once
#include <stdint.h>

// The board's date and time (UTC), for what is stamped with it: the capture
// files' wallSec, the log. The car has no clock the board can read; the head
// unit's 0x9D carries hours and minutes only (the panel's clock), so the date
// comes from MegaOpen Companion (the alerts poll's `t`, the USB link's HELLO)
// or the iPhone's CTS.
namespace WallClock
{
    // ms since 1970 from an app. Set when the clock is unset or more than
    // 5 s off; one line in the log when it is. False: not a plausible time.
    bool fromApp(int64_t epochMs, const char* via);
    // A date has been set this boot.
    bool known();
}
