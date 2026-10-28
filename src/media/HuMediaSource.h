#pragma once
#include "IMediaSource.h"
#include <stdint.h>
#include <atomic>
#include "../proto/canbox/Protocol.h"
#include "../utils/Locks.h"

// Now-playing text as the head unit sends it. Which frames carry it, and in
// what form, is the protocol's (Canbox::Decoder, HuInbound::text); this holds
// what arrived, splits a single "Artist - Title" line, and says whether the
// head unit is still there.
//
// Portable, so the logic is host-tested.
//
// Threading (docs/design/event-tasks.md, Step 2): the hu task feeds onHeard()
// and onText(); the panel's media screen and httpd read. The strings are
// std::string, which a reader must never copy while the writer reassigns them,
// so every reader on another task uses snapshot(), a copy under the mutex, as
// AmsMediaSource does. The web's setters take the same mutex. Holds are a copy.
//
// The feed never waits for it: the hu task calls onText() in the middle of its
// RX drain, with the next frame's ack possibly due. If a reader holds the
// mutex, the text is kept (STASH_DEPTH pieces, oldest dropped and counted) and
// applied, in order, by the next onText() or service() that gets it.
class HuMediaSource : public IMediaSource {
public:
    static constexpr uint8_t TEXT_MAX = 64;

    // Order of the halves when a single line carries both (HuText::Line).
    void setTitleFirst(bool on);
    bool titleFirst() const { return _titleFirst; }
    // Clears what was shown (the web's "forget"; the protocol forgets its own).
    void forget();

    // Link timeout, measured from the last frame of any kind (a long track
    // sends no text for minutes). The unit sends text only on a change, and
    // its one periodic frame is the clock, once a minute: the timeout must
    // outlast that or the track drops between clock frames.
    void setLinkTimeoutMs(uint32_t ms) { _linkTimeoutMs = ms; }
    uint32_t linkTimeoutMs() const { return _linkTimeoutMs; }

    // ---- the feed (the hu task) -----------------------------------------------
    // Every frame from the head unit: its liveness.
    void onHeard(uint32_t nowMs);
    // A piece of text (HuInbound::text). The change callback runs after the
    // mutex is released, so it may read snapshot().
    void onText(Canbox::HuText field, const char* text, uint32_t nowMs);
    // Once a pass: applies text a busy mutex held back.
    void service();

    // ---- IMediaSource -----------------------------------------------------------
    // The writing task only (the hu task, and the host tests).
    const MediaInfo& current() const override { return _info; }
    // Any task.
    MediaInfo snapshot() const override;
    bool trySnapshot(MediaInfo& out, uint32_t waitMs) const override;
    bool active() const override;
    const char* statusText() const override;
    const char* name() const override { return "hu"; }

    // The reader supplies the clock, so active() can expire with nothing arriving.
    void setNow(uint32_t nowMs) { _nowMs.store(nowMs, std::memory_order_relaxed); }

    uint32_t frames() const { return _frames; }
    // millis() of the newest frame, 0 = none yet. Any task (/api/health).
    uint32_t lastFrameMs() const { return _lastFrameMs; }
    // A frame within the link timeout, by the caller's clock. Any task: the
    // health chain `hu` and the keys task's volume fallback ask the same.
    bool heard(uint32_t nowMs) const
    {
        const uint32_t last = _lastFrameMs;
        return last && (uint32_t)(nowMs - last) < _linkTimeoutMs;
    }
    // Text the feed found the mutex busy for and kept for later; and of
    // those, the pieces lost because the stash was full. Both should stay
    // near 0: a hold is one string copy.
    uint32_t lockMisses() const { return _lockMisses; }
    uint32_t stashDropped() const { return _stashDropped; }

#ifdef NATIVE
    // The host tests: hold the mutex as a reader on another task would.
    bool testHold()    { return _lock.tryTake(); }
    void testRelease() { _lock.give(); }
#endif

private:
    void publish(const char* artist, const char* title, uint32_t nowMs, const char* album = nullptr);
    void forgetLocked();
    void apply(Canbox::HuText field, const char* text, uint32_t nowMs);
    // Under the mutex: what is stashed, then this piece unless !have.
    void applyLocked(Canbox::HuText field, const char* text, uint32_t nowMs, bool have);
    void stash(Canbox::HuText field, const char* text, uint32_t nowMs);

    // Readers and the web's setters (httpd) may wait this long; the feed never.
    static constexpr uint32_t LOCK_WAIT_MS = 20;
    mutable Locks::Mutex _lock;
    bool     _changed = false;     // set under the lock, notified after it
    uint32_t _lockMisses = 0, _stashDropped = 0;

    static constexpr uint8_t STASH_DEPTH = 2;
    struct Stashed {
        Canbox::HuText field = Canbox::HuText::Title;
        uint32_t atMs = 0;
        char     t[TEXT_MAX] = {};
    };
    Stashed  _stash[STASH_DEPTH];
    uint8_t  _stashN = 0;

    MediaInfo _info;
    bool     _titleFirst = false;
    uint32_t _linkTimeoutMs = 150000;
    volatile uint32_t _lastFrameMs = 0;
    // Two writers (the feed, and the reader's setNow), one word: atomic.
    std::atomic<uint32_t> _nowMs{0};
    uint32_t _frames = 0;
    bool     _everText = false;
};
