#pragma once
#include <stdint.h>
#include <atomic>
#include "Protocol.h"

// Now-playing text in the head unit's Raise frames (Raise 112's decoder).
//
// No vendor spec documents a track-text message (docs/raise-protocol.md
// §"Traps"), but DUDU units do send it, in two forms:
//   * STRUCTURED: `[0x83][field][UTF-16LE text, NUL-padded]` on cmd 0x99,
//     the field named in the frame (observed on a DUDU7). Recognised by the
//     marker, not the cmd byte.
//   * PLAIN: printable bytes on some command. The command is not known, so:
//       - PINNED: a configured cmd (raise.titleCmd / raise.artistCmd); only
//         that one is read.
//       - LEARNING (cmd 0, default): the first cmd seen carrying text becomes
//         the title channel, the next different one the artist channel.
//         Sticky for the session, so a text-like status frame cannot flip them.
// A single channel carrying "Artist - Title" comes out as HuText::Line.
//
// The hu task feeds onFrame(); forget() and the reads are any task's (atomics).
// Portable, so the parsing is host-tested.
namespace Canbox
{
    class RaiseText {
    public:
        static constexpr uint8_t TEXT_MAX = 64;
        // Text = at least TEXT_MIN_LEN bytes and at least half printable (titles
        // often carry a length byte, codepage marker or NUL).
        static constexpr uint8_t TEXT_MIN_LEN = 4;
        static bool looksLikeText(const uint8_t* payload, uint8_t len);

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
        // Payload -> NUL-terminated ASCII: printable bytes kept, the rest
        // dropped, trimmed.
        static void extract(const uint8_t* payload, uint8_t len, char* out, uint8_t cap);

        // The hu task, from Raise's tuning. 0 = learn. A change forgets what
        // was learned and returns true: what is shown is stale (HuText::Clear).
        bool setChannels(uint8_t titleCmd, uint8_t artistCmd);
        // Any task: forget the learned channels; the next text frames learn again.
        void forget()
        {
            _learnedTitle.store(0, std::memory_order_relaxed);
            _learnedArtist.store(0, std::memory_order_relaxed);
        }

        // Any task.
        uint8_t  titleCmd() const        { return _titleCmd.load(std::memory_order_relaxed); }
        uint8_t  artistCmd() const       { return _artistCmd.load(std::memory_order_relaxed); }
        uint8_t  learnedTitleCmd() const { return _learnedTitle.load(std::memory_order_relaxed); }
        uint8_t  learnedArtistCmd() const{ return _learnedArtist.load(std::memory_order_relaxed); }
        uint32_t textFrames() const      { return _textFrames.load(std::memory_order_relaxed); }

        // Every good frame of the head unit's. The hu task. `text` gets what
        // the frame carries, if anything.
        using TextFn = void (*)(HuText field, const char* text, void* ctx);
        void onFrame(uint8_t cmd, const uint8_t* payload, uint8_t len, TextFn text, void* ctx);

    private:
        std::atomic<uint8_t>  _titleCmd{0}, _artistCmd{0};          // pinned, 0 = learn
        std::atomic<uint8_t>  _learnedTitle{0}, _learnedArtist{0};  // what learning settled on
        std::atomic<uint32_t> _textFrames{0};
    };
}
