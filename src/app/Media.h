#pragma once
#include "../Features.h"
#if FEATURE_DISPLAY_CAN || FEATURE_BLE
#include "../cfg/Settings.h"
#include "../media/MediaRouter.h"
#include "../media/HuMediaSource.h"

// Now-playing, source-neutral: the phone (AMS, added by the BLE start) and the
// head unit (HuMediaSource); `mediaSource` picks which one reaches the glass.
namespace App
{
    namespace Media
    {
        // The head unit registered, its title byte and the saved mode.
        void begin(const Cfg::Settings& s);
        MediaRouter&   router();
        HuMediaSource& hu();
        // A source's change callback: only the router's active source reaches
        // the screen (the ui task's mediaChanged()). Any task.
        void onChange(IMediaSource& src, void*);
    }
}
#endif
