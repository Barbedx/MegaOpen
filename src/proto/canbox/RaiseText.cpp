#include "RaiseText.h"
#include <string.h>

namespace Canbox
{
    bool RaiseText::looksLikeText(const uint8_t* payload, uint8_t len)
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

    bool RaiseText::decodeWide(const uint8_t* p, uint8_t len, char* out, uint8_t cap)
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

    void RaiseText::extract(const uint8_t* payload, uint8_t len, char* out, uint8_t cap)
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

    bool RaiseText::setChannels(uint8_t titleCmd, uint8_t artistCmd)
    {
        if (titleCmd == this->titleCmd() && artistCmd == this->artistCmd()) return false;
        _titleCmd.store(titleCmd, std::memory_order_relaxed);
        _artistCmd.store(artistCmd, std::memory_order_relaxed);
        // Learned channels are stale under a new assignment, and so is the text.
        _learnedTitle.store(0, std::memory_order_relaxed);
        _learnedArtist.store(0, std::memory_order_relaxed);
        return true;
    }

    void RaiseText::onFrame(uint8_t cmd, const uint8_t* payload, uint8_t len, TextFn text, void* ctx)
    {
        if (!payload) return;
        const uint8_t pinT = titleCmd(), pinA = artistCmd();
        // A pinned channel filters out every other command, structured branch included.
        if (pinT && cmd != pinT && cmd != pinA) return;

        // ---- the structured form ----------------------------------------------
        // Checked first: UTF-16 text fails looksLikeText(). payload[1] names the
        // field, so no learning is needed.
        if (len >= 4 && payload[0] == MARKER_TEXT) {
            char t[TEXT_MAX];
            // Field 01 is binary (track number, total), never text: not decoded.
            if (payload[1] == FIELD_TRACK) return;
            if (!decodeWide(payload + 2, (uint8_t)(len - 2), t, sizeof(t))) return;
            _textFrames.fetch_add(1, std::memory_order_relaxed);
            // Recorded for diagnostics only.
            if (pinT == 0 && learnedTitleCmd() == 0) _learnedTitle.store(cmd, std::memory_order_relaxed);
            if (!text) return;
            if      (payload[1] == FIELD_TITLE)  text(HuText::Title, t, ctx);
            else if (payload[1] == FIELD_ARTIST) text(HuText::Artist, t, ctx);
            else if (payload[1] == FIELD_ALBUM)  text(HuText::Album, t, ctx);
            return;
        }

        if (!looksLikeText(payload, len)) return;
        char t[TEXT_MAX];
        extract(payload, len, t, sizeof(t));
        if (!t[0]) return;
        _textFrames.fetch_add(1, std::memory_order_relaxed);

        // ---- learning -----------------------------------------------------------
        // First text command is the title, the next different one the artist; sticky.
        const uint8_t lt = learnedTitleCmd(), la = learnedArtistCmd();
        if (pinT == 0 && lt == 0) _learnedTitle.store(cmd, std::memory_order_relaxed);
        else if (pinA == 0 && la == 0 && cmd != lt && cmd != pinT) _learnedArtist.store(cmd, std::memory_order_relaxed);
        if (!text) return;

        // Single-line units send "Artist - Title" on one channel: the source
        // splits it, but only while no artist channel exists.
        const uint8_t title = pinT ? pinT : learnedTitleCmd();
        const uint8_t artist = pinA ? pinA : learnedArtistCmd();
        if (artist == 0 && strstr(t, " - ")) { text(HuText::Line, t, ctx); return; }
        if (cmd == title)       text(HuText::Title, t, ctx);
        else if (cmd == artist) text(HuText::Artist, t, ctx);
    }
}
