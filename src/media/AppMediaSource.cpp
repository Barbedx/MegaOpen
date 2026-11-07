#include "AppMediaSource.h"
#include <string.h>

uint32_t AppMediaSource::crc32(const uint8_t* d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

namespace
{
    // At most TEXT_MAX bytes, cut on a UTF-8 character boundary.
    void assign(std::string& to, const char* s, bool& changed)
    {
        if (!s) s = "";
        size_t n = strlen(s);
        if (n > AppMediaSource::TEXT_MAX) {
            n = AppMediaSource::TEXT_MAX;
            while (n && ((uint8_t)s[n] & 0xC0) == 0x80) --n;
        }
        if (to.size() == n && to.compare(0, n, s, n) == 0) return;
        to.assign(s, n);
        changed = true;
    }
}

bool AppMediaSource::post(const Update& u, uint32_t nowMs)
{
    bool changed = false;
    bool needArt = false;
    if (!_lock.take(LOCK_WAIT_MS)) return false;   // the app posts again on the next change or in 20 s
    assign(_info.playerName, u.app, changed);
    assign(_info.title, u.title, changed);
    assign(_info.artist, u.artist, changed);
    assign(_info.album, u.album, changed);
    const MediaInfo::PlaybackState ps = u.state == State::Playing ? MediaInfo::PlaybackState::Playing
                                                                   : MediaInfo::PlaybackState::Paused;
    if (ps != _info.playbackState || u.state != _state) changed = true;
    _info.playbackState = ps;
    _info.playbackRate = u.state == State::Playing ? (u.rate > 0.0f ? u.rate : 1.0f) : 0.0f;
    _info.elapsedTime = (float)u.posMs / 1000.0f;
    _info.duration = (float)u.durMs / 1000.0f;
    _info.lastUpdateMs = nowMs;
    if (u.artHash != _artHash.load(std::memory_order_relaxed)) {
        _artHash.store(u.artHash, std::memory_order_relaxed);
        _artHave = false;
        changed = true;
    }
    needArt = u.artHash && !_artHave;
    _state = u.state;
    _lastPostMs = nowMs;
    _nowMs.store(nowMs, std::memory_order_relaxed);
    ++_posts;
    _lock.give();
    if (changed) notifyChange();
    return needArt;
}

bool AppMediaSource::putArt(uint32_t hash, const uint8_t* bits)
{
    if (!bits || !hash || crc32(bits, ART_BYTES) != hash) return false;
    if (!_lock.take(LOCK_WAIT_MS)) return false;
    const bool ours = hash == _artHash.load(std::memory_order_relaxed);
    if (ours) {
        memcpy(_art, bits, ART_BYTES);
        _artHave = true;
    }
    _lock.give();
    if (ours) notifyChange();
    return ours;
}

bool AppMediaSource::art(uint8_t* out) const
{
    if (!_artHave || !_lock.take(LOCK_WAIT_MS)) return false;
    const bool have = _artHave;
    if (have) memcpy(out, _art, ART_BYTES);
    _lock.give();
    return have;
}

MediaInfo AppMediaSource::snapshot() const
{
    MediaInfo copy;
    if (!_lock.take(LOCK_WAIT_MS)) return copy;
    copy = _info;
    _lock.give();
    return copy;
}

bool AppMediaSource::trySnapshot(MediaInfo& out, uint32_t waitMs) const
{
    if (!_lock.take(waitMs)) return false;
    out = _info;
    _lock.give();
    return true;
}

bool AppMediaSource::active() const
{
    const uint32_t last = _lastPostMs;
    if (!_posts || _state == State::None) return false;
    return (uint32_t)(_nowMs.load(std::memory_order_relaxed) - last) < LIVE_MS;
}

const char* AppMediaSource::statusText() const
{
    if (!_posts) return "NO APP";
    if ((uint32_t)(_nowMs.load(std::memory_order_relaxed) - _lastPostMs) >= LIVE_MS) return "APP QUIET";
    if (_state == State::None) return "APP: NOTHING PLAYS";
    return "";
}
