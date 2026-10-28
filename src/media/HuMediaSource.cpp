#include "HuMediaSource.h"
#include <strings.h>
#include <string.h>

using Canbox::HuText;

MediaInfo HuMediaSource::snapshot() const
{
    MediaInfo copy;
    if (!_lock.take(LOCK_WAIT_MS)) return copy;
    copy = _info;
    _lock.give();
    return copy;
}

bool HuMediaSource::trySnapshot(MediaInfo& out, uint32_t waitMs) const
{
    if (!_lock.take(waitMs)) return false;
    out = _info;
    _lock.give();
    return true;
}

void HuMediaSource::setTitleFirst(bool on)
{
    if (!_lock.take(LOCK_WAIT_MS)) return;
    _titleFirst = on;
    _lock.give();
}

void HuMediaSource::forget()
{
    if (!_lock.take(LOCK_WAIT_MS)) return;
    forgetLocked();
    _lock.give();
}

void HuMediaSource::forgetLocked()
{
    _everText = false;
    _info.title.clear();
    _info.artist.clear();
    _info.album.clear();
}

// The head unit's own placeholder for a Bluetooth source with no name (seen as
// "Unknown ContactE" on a DUDU, 2026-10-26): not a song, never on the glass.
// The owner's call: this one only. Any tail after it (that E) goes with it.
static bool placeholder(const char* t)
{
    static const char P[] = "Unknown Contact";
    return t && strncasecmp(t, P, sizeof(P) - 1) == 0;
}

void HuMediaSource::publish(const char* artist, const char* title, uint32_t nowMs, const char* album)
{
    if (placeholder(title)) title = nullptr;
    if (placeholder(artist)) artist = nullptr;
    bool changed = false;
    if (title  && _info.title  != title)  { _info.title  = title;  changed = true; }
    if (artist && _info.artist != artist) { _info.artist = artist; changed = true; }
    if (album  && _info.album  != album)  { _info.album  = album;  changed = true; }
    if (!changed) return;

    _info.lastUpdateMs = nowMs;
    _everText = true;
    _changed = true;
}

void HuMediaSource::apply(HuText field, const char* text, uint32_t nowMs)
{
    switch (field) {
        case HuText::Title:  publish(nullptr, text, nowMs); return;
        case HuText::Artist: publish(text, nullptr, nowMs); return;
        case HuText::Album:  publish(nullptr, nullptr, nowMs, text); return;
        case HuText::Clear:  forgetLocked(); return;
        case HuText::Line:   break;
    }
    // "Artist - Title" on one line; without the separator it is the title.
    const char* sep = strstr(text, " - ");
    if (!sep) { publish(nullptr, text, nowMs); return; }
    char left[TEXT_MAX], right[TEXT_MAX];
    const size_t ln = (size_t)(sep - text);
    const size_t n = ln < sizeof(left) - 1 ? ln : sizeof(left) - 1;
    memcpy(left, text, n);
    left[n] = '\0';
    strncpy(right, sep + 3, sizeof(right) - 1);
    right[sizeof(right) - 1] = '\0';
    if (_titleFirst) publish(right, left, nowMs);
    else             publish(left, right, nowMs);
}

void HuMediaSource::onHeard(uint32_t nowMs)
{
    ++_frames;
    // Every frame counts as link liveness, not only text frames.
    _lastFrameMs = nowMs;
    _nowMs.store(nowMs, std::memory_order_relaxed);
}

void HuMediaSource::onText(HuText field, const char* text, uint32_t nowMs)
{
    if (field != HuText::Clear && !text) return;
    // A try-take: the hu task must not wait on a reader (see the class note).
    if (!_lock.tryTake()) {
        ++_lockMisses;
        stash(field, text, nowMs);
        return;
    }
    applyLocked(field, text, nowMs, true);
}

void HuMediaSource::service()
{
    if (!_stashN || !_lock.tryTake()) return;
    applyLocked(HuText::Clear, nullptr, 0, false);
}

void HuMediaSource::applyLocked(HuText field, const char* text, uint32_t nowMs, bool have)
{
    _changed = false;
    // Held-back pieces first: they arrived before this one.
    for (uint8_t i = 0; i < _stashN; ++i)
        apply(_stash[i].field, _stash[i].t, _stash[i].atMs);
    _stashN = 0;
    if (have) apply(field, text, nowMs);
    const bool changed = _changed;
    _lock.give();
    // Outside the lock: the callback may take a snapshot.
    if (changed) notifyChange();
}

void HuMediaSource::stash(HuText field, const char* text, uint32_t nowMs)
{
    if (_stashN == STASH_DEPTH) {
        // Keep the newest: an older title is about to be replaced anyway.
        for (uint8_t i = 1; i < STASH_DEPTH; ++i) _stash[i - 1] = _stash[i];
        --_stashN;
        ++_stashDropped;
    }
    Stashed& s = _stash[_stashN++];
    s.field = field;
    s.atMs  = nowMs;
    if (text) { strncpy(s.t, text, sizeof(s.t) - 1); s.t[sizeof(s.t) - 1] = '\0'; }
    else      s.t[0] = '\0';
}

bool HuMediaSource::active() const
{
    if (!_everText) return false;
    const uint32_t last = _lastFrameMs;
    if (last == 0) return false;
    return (uint32_t)(_nowMs.load(std::memory_order_relaxed) - last) < _linkTimeoutMs;
}

const char* HuMediaSource::statusText() const
{
    const uint32_t last = _lastFrameMs;
    if (last == 0) return "NO HEAD UNIT";
    if ((uint32_t)(_nowMs.load(std::memory_order_relaxed) - last) >= _linkTimeoutMs) return "HEAD UNIT QUIET";
    if (!_everText) return "HU: NO TEXT YET";
    return "";
}
