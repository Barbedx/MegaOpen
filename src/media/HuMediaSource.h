#pragma once
#include "IMediaSource.h"
#include <stdint.h>
#include <atomic>
#include "../utils/Locks.h"

// Now-playing text as the head unit sends it up the Raise UART.
//
// No vendor spec documents a track-text message (docs/raise-protocol.md
// §"Traps"), but DUDU units do send it. The command byte is not known, so:
//   * PINNED: a configured cmd; only that one is read.
//   * LEARNING (cmd 0, default): the first cmd seen carrying text becomes the
//     title channel, the next different one the artist channel. Sticky for the
//     session, so a text-like status frame cannot flip the rows.
//
// Portable, so the parsing is host-tested.
//
// Threading (docs/design/event-tasks.md, Step 2): the hu task feeds onFrame();
// the panel's media screen and httpd read. The strings are std::string, which
// a reader must never copy while the writer reassigns them, so every reader
// on another task uses snapshot(), a copy under the mutex, as AmsMediaSource
// does. The web's setters take the same mutex. Holds are a copy.
//
// The feed never waits for it: the hu task calls onFrame() in the middle of
// its RX drain, with the next frame's ack possibly due. If a reader holds the
// mutex, the frame is kept (STASH_DEPTH of them, oldest dropped and counted)
// and applied, in order, by the next onFrame() or service() that gets it.
class HuMediaSource : public IMediaSource {
public:
    static constexpr uint8_t TEXT_MAX = 64;

    // Text = at least TEXT_MIN_LEN bytes and at least half printable (titles
    // often carry a length byte, codepage marker or NUL).
    static constexpr uint8_t TEXT_MIN_LEN = 4;
    static bool looksLikeText(const uint8_t* payload, uint8_t len);

    // ---- the structured form, observed on a DUDU7 -------------------------
    // `[0x83][field][UTF-16LE text, NUL-padded to a fixed width]` on cmd 0x99.
    // Recognised by the marker, not the cmd byte.
    // Must be tried before looksLikeText(), which rejects it (12 printable
    // bytes in a 34-byte frame).
    static constexpr uint8_t MARKER_TEXT  = 0x83;
    // The unit sends at most 15 UTF-16 characters per field (k0.a3.c3): the
    // rest of a long title never reaches the wire.
    static constexpr uint8_t FIELD_TRACK  = 0x01;   // track number + total, LE16, binary
    static constexpr uint8_t FIELD_TITLE  = 0x02;
    static constexpr uint8_t FIELD_ARTIST = 0x03;
    static constexpr uint8_t FIELD_ALBUM  = 0x04;

    // UTF-16LE -> a NUL-terminated UTF-8 line (Latin Extended-A folded to its
    // base letter). False when the payload is not whole code units or decodes
    // to nothing. Transliteration for the panel is the display layer's job.
    static bool decodeWide(const uint8_t* p, uint8_t len, char* out, uint8_t cap);

    // 0 = learn. Changing either clears what was learned.
    void setChannels(uint8_t titleCmd, uint8_t artistCmd);
    uint8_t titleCmd()  const { return _titleCmd; }
    uint8_t artistCmd() const { return _artistCmd; }
    // What learning settled on; 0 until text arrives.
    uint8_t learnedTitleCmd()  const { return _learnedTitle; }
    uint8_t learnedArtistCmd() const { return _learnedArtist; }
    void forget();

    // Order of the halves when a single channel carries both.
    void setTitleFirst(bool on);
    bool titleFirst() const { return _titleFirst; }

    // Link timeout, measured from the last frame of any kind (a long track
    // sends no text for minutes). The unit sends text only on a change, and
    // its one periodic frame is the 0x9D clock, once a minute: the timeout
    // must outlast that or the track drops between clock frames.
    void setLinkTimeoutMs(uint32_t ms) { _linkTimeoutMs = ms; }
    uint32_t linkTimeoutMs() const { return _linkTimeoutMs; }

    // ---- the feed ----------------------------------------------------------
    // Every inbound Raise frame, in RxParser order. The hu task. The change
    // callback runs after the mutex is released, so it may read snapshot().
    void onFrame(uint8_t cmd, const uint8_t* payload, uint8_t len, uint32_t nowMs);
    // The hu task, once a pass: applies frames a busy mutex held back.
    void service();

    // ---- IMediaSource -------------------------------------------------------
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

    uint32_t textFrames() const { return _textFrames; }
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
    // Frames the feed found the mutex busy for and kept for later; and of
    // those, the ones lost because the stash was full. Both should stay near
    // 0: a hold is one string copy.
    uint32_t lockMisses() const { return _lockMisses; }
    uint32_t stashDropped() const { return _stashDropped; }

#ifdef NATIVE
    // The host tests: hold the mutex as a reader on another task would.
    bool testHold()    { return _lock.tryTake(); }
    void testRelease() { _lock.give(); }
#endif

private:
    // Payload -> NUL-terminated ASCII: printable bytes kept, the rest dropped,
    // trimmed. Transliteration is the display layer's job.
    static void extract(const uint8_t* payload, uint8_t len, char* out, uint8_t cap);
    void assign(uint8_t cmd, const char* text, uint32_t nowMs);
    void publish(const char* artist, const char* title, uint32_t nowMs, const char* album = nullptr);
    void forgetLocked();
    void parse(uint8_t cmd, const uint8_t* payload, uint8_t len, uint32_t nowMs);

    // Under the mutex: what is stashed, then this frame (may be null).
    void applyLocked(uint8_t cmd, const uint8_t* payload, uint8_t len, uint32_t nowMs);
    void stash(uint8_t cmd, const uint8_t* payload, uint8_t len, uint32_t nowMs);

    // Readers and the web's setters (httpd) may wait this long; the feed never.
    static constexpr uint32_t LOCK_WAIT_MS = 20;
    mutable Locks::Mutex _lock;
    bool     _changed = false;     // set under the lock, notified after it
    uint32_t _lockMisses = 0, _stashDropped = 0;

    // Frames held back by a busy mutex. 130 bytes covers the longest text
    // field either form can show (2 header bytes + 63 UTF-16 code units); a
    // longer payload is cut there, which only shortens what extract() reads.
    static constexpr uint8_t STASH_DEPTH = 2;
    static constexpr uint8_t STASH_BYTES = 130;
    struct Stashed {
        uint8_t  cmd = 0, len = 0;
        uint32_t atMs = 0;
        uint8_t  p[STASH_BYTES];
    };
    Stashed  _stash[STASH_DEPTH];
    uint8_t  _stashN = 0;

    MediaInfo _info;
    uint8_t  _titleCmd = 0, _artistCmd = 0;         // pinned, 0 = learn
    uint8_t  _learnedTitle = 0, _learnedArtist = 0; // what learning settled on
    bool     _titleFirst = false;
    uint32_t _linkTimeoutMs = 150000;
    volatile uint32_t _lastFrameMs = 0;
    // Two writers (the feed, and the reader's setNow), one word: atomic.
    std::atomic<uint32_t> _nowMs{0};
    uint32_t _frames = 0, _textFrames = 0;
    bool     _everText = false;
};
