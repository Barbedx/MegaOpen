#include "Media.h"
#if FEATURE_DISPLAY_CAN || FEATURE_BLE
#include "../utils/Log.h"
#if FEATURE_DISPLAY_CAN
#include "UiTask.h"
#endif

namespace App
{
    namespace Media
    {
        namespace
        {
            // Source-neutral now-playing: phone (AMS) and head unit (HuMediaSource);
            // `mediaSource` in NVS picks which one reaches the glass.
            MediaRouter   s_router;
            HuMediaSource s_hu;
            // Now playing from the head unit's Android, posted by MegaOpen Companion.
            AppMediaSource s_app;

            // Only the router's active source reaches the screen. May run on the NimBLE host
            // task, so it only sets a flag: MediaScreen is touched only by its owner (the
            // ui task, App::Ui; a concurrent copy would let compose()'s strlen run past an
            // unterminated title).
            void onChangeImpl(IMediaSource& src)
            {
#if FEATURE_DISPLAY_CAN
                if (s_router.activeSource() == &src) App::Ui::mediaChanged();
#else
                (void)src;
#endif
            }
        }

        MediaRouter&   router() { return s_router; }
        HuMediaSource& hu()     { return s_hu; }
        AppMediaSource& app()   { return s_app; }
        void onChange(IMediaSource& src, void*) { onChangeImpl(src); }

        void begin(const Cfg::Settings& s)
        {
            // The head unit as a media source, always registered; `mediaSource` selects.
            // Command bytes from NVS; 0 = learn which carries the title (HuMediaSource).
            s_hu.setTitleFirst(s.huTitleFirst);
            s_hu.setChangeCallback(onChange, nullptr);
            // The AMS slot is filled by the BLE block below, if it starts.
            s_router.setSources(nullptr, &s_hu);
            s_app.setChangeCallback(onChange, nullptr);
            s_router.setApp(&s_app);
            {
                MediaRouter::Mode m = MediaRouter::modeFromStr(s.mediaSource.c_str());
#if !FEATURE_BLE
                // No radio in this image, and `ams` is the stored default: force `hu`.
                if (m == MediaRouter::Mode::Ams) {
                    LOGW("media", "source: NVS says 'ams', but this build has no radio "
                                 "— forcing 'hu'");
                    m = MediaRouter::Mode::Hu;
                }
#endif
                s_router.setMode(m);
            }
        }
    }
}
#endif
