#include "HuMediaSource.h"
#include <strings.h>
#include <string.h>

bool HuMediaSource::looksLikeText(const uint8_t* payload, uint8_t len)
{
    if (!payload || len < TEXT_MIN_LEN) return false;
    uint8_t printable = 0;
    for (uint8_t i = 0; i < len; ++i)
        if (payload[i] >= 0x20 && payload[i] < 0x7F) ++printable;
    return (uint16_t)printable * 2 >= (uint16_t)len;
}

// Latin Extended-A (U+0100-017F) folded to its base letter ("Wieże" -> "Wieze").
// Latin-1 passes through: the display layer has its own table for that range.
static const char kLatinExtA[128] = {
    'A','a','A','a','A','a','C','c','C','c','C','c','C','c','D','d',
    'D','d','E','e','E','e','E','e','E','e','E','e','G','g','G','g',
    'G','g','G','g','H','h','H','h','I','i','I','i','I','i','I','i',
    'I','i','I','i','J','j','K','k','k','L','l','L','l','L','l','L',
    'l','L','l','N','n','N','n','N','n','n','N','n','O','o','O','o',
    'O','o','O','o','R','r','R','r','R','r','S','s','S','s','S','s',
    'S','s','T','t','T','t','T','t','U','u','U','u','U','u','U','u',
    'U','u','U','u','W','w','Y','y','Y','Z','z','Z','z','Z','z','s',
};

// One UTF-16 code unit -> UTF-8 bytes in out (up to 3), or 0 to drop it.
// Latin Extended-A folds to its base letter here; everything else (Cyrillic
// above all) stays as UTF-8 for the display layer's transliteration and for
// the web, which shows it as is. Surrogate halves (emoji) are dropped.
static uint8_t foldWide(uint16_t u, char* out)
{
    if (u < 0x20)  return 0;                       // controls, including the NUL pad
    if (u < 0x80)  { out[0] = (char)u; return 1; } // ASCII
    if (u < 0xA0)  return 0;                       // C1 controls
    if (u >= 0x0100 && u <= 0x017F) { out[0] = kLatinExtA[u - 0x0100]; return 1; }
    if (u >= 0xD800 && u <= 0xDFFF) return 0;
    if (u < 0x800) {
        out[0] = (char)(0xC0 | (u >> 6));
        out[1] = (char)(0x80 | (u & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (u >> 12));
    out[1] = (char)(0x80 | ((u >> 6) & 0x3F));
    out[2] = (char)(0x80 | (u & 0x3F));
    return 3;
}

bool HuMediaSource::decodeWide(const uint8_t* p, uint8_t len, char* out, uint8_t cap)
{
    if (!p || cap == 0) return false;
    if (cap) out[0] = '\0';
    // Fixed-width field: trim NUL padding in whole code units, not bytes.
    while (len >= 2 && !p[len - 1] && !p[len - 2]) len = (uint8_t)(len - 2);
    if (len < 2 || (len & 1)) return false;

    uint8_t k = 0;
    for (uint8_t i = 0; (uint8_t)(i + 1) < len && k < cap - 1; i = (uint8_t)(i + 2)) {
        const uint16_t u = (uint16_t)p[i] | ((uint16_t)p[i + 1] << 8);
        if (!u) break;                              // embedded terminator
        char c[3];
        const uint8_t n = foldWide(u, c);
        if (k + n > cap - 1) break;                 // never split a character
        for (uint8_t j = 0; j < n; ++j) out[k++] = c[j];
    }
    out[k] = '\0';

    // Trim spaces, as extract() does.
    uint8_t a = 0;
    while (out[a] == ' ') ++a;
    if (a) { memmove(out, out + a, (size_t)(k - a) + 1); k = (uint8_t)(k - a); }
    while (k && out[k - 1] == ' ') out[--k] = '\0';
    return k > 0;
}

void HuMediaSource::extract(const uint8_t* payload, uint8_t len, char* out, uint8_t cap)
{
    if (cap == 0) return;
    uint8_t k = 0;
    for (uint8_t i = 0; i < len && k < cap - 1; ++i) {
        const uint8_t c = payload[i];
        if (c >= 0x20 && c < 0x7F) out[k++] = (char)c;
    }
    out[k] = '\0';

    // Trim padding (and a printable length byte) so the title does not scroll longer.
    uint8_t a = 0;
    while (out[a] == ' ') ++a;
    if (a) {
        memmove(out, out + a, (size_t)(k - a) + 1);
        k = (uint8_t)(k - a);
    }
    while (k && out[k - 1] == ' ') out[--k] = '\0';
}

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

void HuMediaSource::setChannels(uint8_t titleCmd, uint8_t artistCmd)
{
    if (!_lock.take(LOCK_WAIT_MS)) return;
    if (titleCmd != _titleCmd || artistCmd != _artistCmd) {
        _titleCmd = titleCmd;
        _artistCmd = artistCmd;
        // Learned channels are stale under a new assignment.
        forgetLocked();
    }
    _lock.give();
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
    _learnedTitle = 0;
    _learnedArtist = 0;
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

void HuMediaSource::assign(uint8_t cmd, const char* text, uint32_t nowMs)
{
    // Single-line units send "Artist - Title" on one channel; split it, but only
    // while no artist channel exists.
    const bool onlyChannel = (_artistCmd == 0 && _learnedArtist == 0);
    const char* sep = onlyChannel ? strstr(text, " - ") : nullptr;
    if (sep) {
        char left[TEXT_MAX], right[TEXT_MAX];
        const size_t ln = (size_t)(sep - text);
        const size_t cap = sizeof(left) - 1;
        const size_t n = ln < cap ? ln : cap;
        memcpy(left, text, n);
        left[n] = '\0';
        strncpy(right, sep + 3, sizeof(right) - 1);
        right[sizeof(right) - 1] = '\0';
        if (_titleFirst) publish(right, left, nowMs);
        else             publish(left, right, nowMs);
        return;
    }

    if (cmd == _titleCmd || (_titleCmd == 0 && cmd == _learnedTitle)) {
        publish(nullptr, text, nowMs);
    } else if (cmd == _artistCmd || (_artistCmd == 0 && cmd == _learnedArtist)) {
        publish(text, nullptr, nowMs);
    }
}

void HuMediaSource::onFrame(uint8_t cmd, const uint8_t* payload, uint8_t len, uint32_t nowMs)
{
    ++_frames;
    // Every frame counts as link liveness, not only text frames.
    _lastFrameMs = nowMs;
    _nowMs.store(nowMs, std::memory_order_relaxed);

    // A try-take: the hu task must not wait on a reader (see the class note).
    if (!_lock.tryTake()) {
        ++_lockMisses;
        stash(cmd, payload, len, nowMs);
        return;
    }
    applyLocked(cmd, payload, len, nowMs);
}

void HuMediaSource::service()
{
    if (!_stashN || !_lock.tryTake()) return;
    applyLocked(0, nullptr, 0, 0);
}

void HuMediaSource::applyLocked(uint8_t cmd, const uint8_t* payload, uint8_t len, uint32_t nowMs)
{
    _changed = false;
    // Held-back frames first: they arrived before this one.
    for (uint8_t i = 0; i < _stashN; ++i)
        parse(_stash[i].cmd, _stash[i].p, _stash[i].len, _stash[i].atMs);
    _stashN = 0;
    if (payload) parse(cmd, payload, len, nowMs);
    const bool changed = _changed;
    _lock.give();
    // Outside the lock: the callback may take a snapshot.
    if (changed) notifyChange();
}

void HuMediaSource::stash(uint8_t cmd, const uint8_t* payload, uint8_t len, uint32_t nowMs)
{
    if (!payload) return;
    if (_stashN == STASH_DEPTH) {
        // Keep the newest: an older title is about to be replaced anyway.
        for (uint8_t i = 1; i < STASH_DEPTH; ++i) _stash[i - 1] = _stash[i];
        --_stashN;
        ++_stashDropped;
    }
    Stashed& s = _stash[_stashN++];
    s.cmd  = cmd;
    s.len  = len < STASH_BYTES ? len : STASH_BYTES;
    s.atMs = nowMs;
    memcpy(s.p, payload, s.len);
}

void HuMediaSource::parse(uint8_t cmd, const uint8_t* payload, uint8_t len, uint32_t nowMs)
{
    // A pinned channel filters out every other command, structured branch included.
    if (_titleCmd && cmd != _titleCmd && cmd != _artistCmd) return;

    // ---- the structured form ----------------------------------------------
    // Checked first: UTF-16 text fails looksLikeText(). payload[1] names the field,
    // so no learning is needed.
    if (len >= 4 && payload[0] == MARKER_TEXT) {
        char text[TEXT_MAX];
        // Field 01 is binary (track number, total), never text: not decoded.
        if (payload[1] == FIELD_TRACK) return;
        if (!decodeWide(payload + 2, (uint8_t)(len - 2), text, sizeof(text))) return;
        ++_textFrames;
        // Recorded for diagnostics only.
        if (_titleCmd == 0 && _learnedTitle == 0) _learnedTitle = cmd;
        if      (payload[1] == FIELD_TITLE)  publish(nullptr, text, nowMs);
        else if (payload[1] == FIELD_ARTIST) publish(text, nullptr, nowMs);
        else if (payload[1] == FIELD_ALBUM)  publish(nullptr, nullptr, nowMs, text);
        return;
    }

    if (!looksLikeText(payload, len)) return;

    char text[TEXT_MAX];
    extract(payload, len, text, sizeof(text));
    if (!text[0]) return;

    ++_textFrames;

    // ---- learning ---------------------------------------------------------
    // First text command is the title, the next different one the artist; sticky.
    if (_titleCmd == 0 && _learnedTitle == 0) _learnedTitle = cmd;
    else if (_artistCmd == 0 && _learnedArtist == 0 && cmd != _learnedTitle &&
             cmd != _titleCmd)
        _learnedArtist = cmd;

    assign(cmd, text, nowMs);
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
