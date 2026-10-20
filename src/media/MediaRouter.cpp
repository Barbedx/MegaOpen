#include "MediaRouter.h"
#include <string.h>

namespace { const MediaInfo kEmpty{}; }

// Ids match the sources' name(); "none" is sourceName() with no source.
const MediaRouter::SourceRow MediaRouter::SOURCES[SOURCE_ROWS] = {
    {"auto", "авто",   true,  false, Mode::Auto},
    {"hu",   "ГУ",     true,  false, Mode::Hu},
    {"ams",  "iPhone", true,  true,  Mode::Ams},
    {"none", "нічого", false, false, Mode::Ams},
};

// The pickable rows of SOURCES, as Modes; anything else is AMS.
MediaRouter::Mode MediaRouter::modeFromStr(const char* s)
{
    if (s) for (const auto& r : SOURCES) if (r.pick && !strcmp(s, r.id)) return r.mode;
    return Mode::Ams;
}

const char* MediaRouter::modeStr(Mode m)
{
    for (const auto& r : SOURCES) if (r.pick && r.mode == m) return r.id;
    return "ams";
}

IMediaSource* MediaRouter::activeSource() const
{
    switch (_mode) {
        case Mode::Hu:   return _hu;
        // The head unit while it talks; the phone only when it is actually
        // connected, so a quiet unit never hands the glass to a phone that
        // is not there ("NO PHONE" instead of the last track).
        case Mode::Auto:
            if (_hu && _hu->active()) return _hu;
            if (_ams && _ams->active()) return _ams;
            return _hu ? _hu : _ams;
        default:         return _ams;
    }
}

const MediaInfo& MediaRouter::current() const
{
    IMediaSource* s = activeSource();
    return s ? s->current() : kEmpty;
}

MediaInfo MediaRouter::snapshot() const
{
    IMediaSource* s = activeSource();
    return s ? s->snapshot() : kEmpty;
}

bool MediaRouter::trySnapshot(MediaInfo& out, uint32_t waitMs) const
{
    IMediaSource* s = activeSource();
    if (!s) { out = kEmpty; return true; }
    return s->trySnapshot(out, waitMs);
}

bool MediaRouter::active() const
{
    IMediaSource* s = activeSource();
    return s && s->active();
}

const char* MediaRouter::statusText() const
{
    IMediaSource* s = activeSource();
    return s ? s->statusText() : "no media source";
}

const char* MediaRouter::sourceName() const
{
    IMediaSource* s = activeSource();
    return s ? s->name() : "none";
}
