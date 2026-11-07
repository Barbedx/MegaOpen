#pragma once
#include "IMediaSource.h"
#include <stdint.h>
#include <atomic>
#include "../utils/Locks.h"

// Now playing from the head unit's Android, as MegaOpen Companion reads it
// (docs/design/companion-media.md): whatever plays there (Spotify, YouTube
// Music, a radio app, DUDU's own player when it publishes a session), with
// the player's name, play state, position and a 48x48 one-bit cover.
//
// The app posts it over HTTP (POST /api/app/media, /api/app/art), so it comes
// the same way over USB, BLE and WiFi. It posts on every change and every
// 20 s while a session exists; a source not heard for LIVE_MS is gone.
//
// Threading: httpd writes (post, putArt); the ui task and httpd read through
// snapshot() / art(), copies under the mutex, as HuMediaSource does.
// Portable, so it is host-tested (test_appmedia).
class AppMediaSource : public IMediaSource {
public:
    static constexpr uint32_t LIVE_MS = 60000;
    static constexpr uint16_t ART_BYTES = 288;   // MediaPane::BYTES: 48x48, 1 bit
    static constexpr size_t   TEXT_MAX = 96;

    enum class State : uint8_t { None, Paused, Playing };

    struct Update {
        const char* app = "";        // the player's label
        const char* title = "";
        const char* artist = "";
        const char* album = "";
        State    state = State::None;
        uint32_t posMs = 0;          // as of the post
        uint32_t durMs = 0;          // 0 = unknown
        float    rate = 1.0f;
        uint32_t artHash = 0;        // CRC-32 of the cover's bits; 0 = no cover
    };

    // httpd. True when the cover named by u.artHash is not here yet: the
    // reply asks the app for it (POST /api/app/art).
    bool post(const Update& u, uint32_t nowMs);
    // httpd: the cover's bits. False (and nothing kept) when their CRC-32 is
    // not `hash`, or `hash` is not the cover the last post named.
    bool putArt(uint32_t hash, const uint8_t* bits);
    // Any task: the current cover, copied. False: none for this track.
    bool art(uint8_t* out) const;
    // Any task: the current cover's hash, 0 = none (the ui task redraws on a change).
    uint32_t artHash() const { return _artHave ? _artHash.load(std::memory_order_relaxed) : 0; }

    static uint32_t crc32(const uint8_t* d, size_t n);   // as java.util.zip.CRC32

    // ---- IMediaSource ---------------------------------------------------------
    const MediaInfo& current() const override { return _info; }   // httpd only
    MediaInfo snapshot() const override;
    bool trySnapshot(MediaInfo& out, uint32_t waitMs) const override;
    bool active() const override;
    const char* statusText() const override;
    const char* name() const override { return "app"; }

    // The reader supplies the clock, so active() can expire with nothing arriving.
    void setNow(uint32_t nowMs) { _nowMs.store(nowMs, std::memory_order_relaxed); }
    uint32_t posts() const { return _posts; }
    uint32_t lastPostMs() const { return _lastPostMs; }
    State state() const { return _state; }

private:
    static constexpr uint32_t LOCK_WAIT_MS = 20;
    mutable Locks::Mutex _lock;
    MediaInfo _info;
    volatile State    _state = State::None;
    volatile uint32_t _lastPostMs = 0;
    volatile uint32_t _posts = 0;
    std::atomic<uint32_t> _nowMs{0};
    // The cover: the hash the last post named, and the bits once they came.
    std::atomic<uint32_t> _artHash{0};
    volatile bool _artHave = false;
    uint8_t  _art[ART_BYTES] = {};
};
