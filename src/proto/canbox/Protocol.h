#pragma once
#include <stddef.h>
#include <stdint.h>
#include "FieldMap.h"
#include "FieldView.h"
#include "../../keys/KeyAction.h"

namespace Cfg { struct SEntry; }

// A head-unit protocol (docs/design/rules.md, slice 0; hu-mapping.md, step 5):
// everything the firmware knows about the head unit's wire format sits behind
// this, and the rest (the hu task, the settings registry, the web, the map)
// reads it through activeProtocol() and nothing else. Raise 112 (the RAV4
// profile, Raise112.cpp) is the one there is.
//
// ANOTHER PROTOCOL: a module that implements Protocol and Emitter (its field
// list and defaults, its message table, its settings rows, its frame) and a
// row in Protocols::all(). Nothing outside the module changes.
namespace Canbox
{
    constexpr uint8_t NO_MSG = 0xFF;
    // The longest frame any protocol here builds: the hu task's queues and
    // every caller's buffer are this big.
    constexpr uint8_t HU_FRAME_MAX = 40;

    // One field with everything resolved. min, max and the values sent are
    // the signal's integers; `dec` places the decimal point (speed 4700,
    // dec 2 = 47.00 km/h). A field's id is the name of its signal.
    struct TargetView {
        const char* id;
        const char* group;
        const char* name;
        const char* unit;
        const char* cmd;      // "" when not sent
        const char* why;      // fw=false: why not, for the page; else null
        bool        boolean;
        int32_t     min, max;
        uint8_t     dec;
        bool        fw;       // false: a car value the protocol has no field for
        uint8_t     sig;
        uint8_t     msg;      // the protocol's message that carries it, NO_MSG none
    };

    // A key frame's state byte, the interface's words (a protocol maps them
    // onto its own wire): a knob code carries a detent count instead.
    enum : uint8_t { KEY_RELEASE = 0, KEY_PRESS = 1, KEY_LONG = 2 };

    // One key code the protocol knows, for the page's key list.
    struct KeyCodeView {
        uint8_t     code;
        const char* name;
        const char* conf;     // how sure: "confirmed" .. "conflict"
        const char* note;     // "" when there is nothing worth saying
    };

    // One message the emitter sends on its schedule.
    struct MessageView {
        const char* name;        // "speed 0x7D03"
        uint8_t     cmd;         // the wire command
        uint32_t    intervalMs;  // the schedule's period
        const char* conf;        // how sure the protocol notes are: "confirmed" .. "conflict"
        const char* note;        // "" when there is nothing worth saying
    };

    // What the hu task applies to the emitter as one block, staged by the web.
    // `tuning` is the protocol's own settings struct, kept opaque: only the
    // protocol reads it (tuningAs<T>), and the settings registry edits it
    // through the protocol's rows (Protocol::setting).
    struct HuConfig {
        static constexpr uint8_t MAX_MESSAGES = 32;
        static constexpr uint8_t TUNING_MAX   = 48;

        bool               enabled = false;
        alignas(8) uint8_t tuning[TUNING_MAX] = {};
        bool               msgOn[MAX_MESSAGES];
        FieldMap           fields;       // the owner's field map

        HuConfig() { for (bool& b : msgOn) b = true; }

        template <class T> T& tuningAs()
        {
            static_assert(sizeof(T) <= TUNING_MAX, "a protocol's tuning must fit HuConfig::TUNING_MAX");
            return *reinterpret_cast<T*>(tuning);
        }
        template <class T> const T& tuningAs() const
        {
            static_assert(sizeof(T) <= TUNING_MAX, "a protocol's tuning must fit HuConfig::TUNING_MAX");
            return *reinterpret_cast<const T*>(tuning);
        }
    };

    // A counter an emitter keeps, for /api/status. Any task (a plain read).
    struct EmitterStat { const char* id; uint32_t value; };

    // The protocol's sender and its schedule. The live one is the hu task's
    // and is never read off it, except its counters; previews are built on a
    // scratch one.
    class Emitter {
    public:
        using WriteFn = void (*)(const uint8_t* frame, uint8_t len, void* ctx);
        virtual ~Emitter() = default;

        virtual void begin(WriteFn write, void* ctx) = 0;
        // Enabled, the protocol's tuning, the message switches, the field map.
        virtual void configure(const HuConfig& c) = 0;
        // Run every scheduler against the current state.
        virtual void tick(const FieldView& s, uint32_t nowMs) = 0;
        // Emit message `msg` now, whatever its schedule and switch. False
        // when the message has no data.
        virtual bool sendNow(uint8_t msg, const FieldView& s) = 0;
        // The head unit asked for a message by its wire command (Raise 0x90).
        // False when there is no such message or nothing to say.
        virtual bool sendByCmd(uint8_t cmd, const FieldView& s) = 0;
        // The frame message `msg` would carry now, without sending it.
        virtual bool preview(uint8_t msg, const FieldView& s, uint8_t* frame, uint8_t& len) const = 0;

        virtual uint32_t    framesSent() const = 0;
        virtual uint8_t     statCount() const { return 0; }
        virtual EmitterStat stat(uint8_t) const { return {"", 0}; }
    };

    // A piece of now-playing text from the head unit. Line: one channel with
    // both, "Artist - Title" (the media source splits it). Clear: what was
    // shown is stale (the channels changed); text is null.
    enum class HuText : uint8_t { Title, Artist, Album, Line, Clear };

    // What the head unit says, as the protocol's decoder hears it. Set once,
    // before the hu task runs; every call is on the hu task, from feed().
    // Any callback may be null.
    struct HuInbound {
        void* ctx = nullptr;
        // A frame just ended and an ack is owed: now, before it is even
        // checked (Raise allows 10 ms; a 12-byte frame takes 4).
        void (*ackDue)(void* ctx) = nullptr;
        // Every good frame: its command, the payload as kept (cut at the
        // decoder's buffer) and the length it declared.
        void (*frame)(uint8_t cmd, const uint8_t* p, uint8_t len, uint16_t total, void* ctx) = nullptr;
        // A byte outside any frame that means nothing to the protocol.
        void (*junk)(uint8_t b, void* ctx) = nullptr;
        // The head unit acknowledged a frame of ours.
        void (*ackIn)(void* ctx) = nullptr;
        // The head unit's clock, range-checked.
        void (*clock)(uint8_t h, uint8_t m, void* ctx) = nullptr;
        // The head unit asked for one of our messages by its wire command.
        void (*request)(uint8_t cmd, void* ctx) = nullptr;
        // Now-playing text, UTF-8, NUL-terminated (null for Clear).
        void (*text)(HuText field, const char* utf8, void* ctx) = nullptr;
    };

    // The decoder's counters. Any task (plain reads).
    struct DecoderStats {
        uint32_t frames = 0, badChecksums = 0, truncated = 0, junkBytes = 0, desyncs = 0;
        // Frames of ours the unit says it does not support (Raise: 0xF3).
        uint32_t notSupported = 0;
        uint32_t textFrames = 0;     // frames that carried now-playing text
    };

    // Where the protocol finds the track text: the command bytes, pinned
    // (the protocol's settings) or learned (0: none yet). Any task.
    struct TextChannels { uint8_t titleCmd = 0, artistCmd = 0, learnedTitle = 0, learnedArtist = 0; };

    // The protocol's receiver: bytes from the head unit in, HuInbound out.
    // The hu task's own.
    class Decoder {
    public:
        virtual ~Decoder() = default;
        virtual void begin(const HuInbound& in) = 0;
        // The protocol's settings, at begin and at every edit. The hu task.
        virtual void configure(const HuConfig& c) = 0;
        virtual void feed(uint8_t b) = 0;
        // Inside a frame: its ack will be owed when it ends, so the lanes
        // hold off (sinks/UartLanes.h).
        virtual bool midFrame() const = 0;
        // Abandon a frame in progress (the line went quiet mid-frame).
        virtual void reset() = 0;
        virtual DecoderStats stats() const = 0;
        // Any task: forget what the text decoding learned (the web's button).
        virtual void         forgetText() = 0;
        virtual TextChannels textChannels() const = 0;
        // What the unit's own frames say about the profile it is set to:
        // a word for /api/health ("toyota", "vw", "unknown"), and whether it
        // is one this protocol does not speak (wired right, set wrong).
        virtual const char* dialect() const = 0;
        virtual bool        wrongDialect() const = 0;
    };

    class Protocol {
    public:
        virtual ~Protocol() = default;
        virtual const char* id() const = 0;            // "raise112"; the page's words are k.<words>.<field>

        // ---- fields: what the head unit can show, each from a car signal ----
        virtual uint8_t     fieldCount() const = 0;
        virtual TargetView  field(uint8_t i) const = 0;
        // The rules a field starts with (FK_OFF for the ones no capture has
        // shown in use); the owner's map goes over them.
        virtual const FieldMap& defaults() const = 0;
        // Why message `msg` cannot go out with these settings (the protocol's
        // own tuning switched it away), an e.<code> for the page; else null.
        virtual const char* messageBlocked(uint8_t msg, const HuConfig& c) const { (void)msg; (void)c; return nullptr; }

        // ---- messages: the emitter's schedule -------------------------------
        virtual uint8_t     messageCount() const = 0;
        virtual MessageView message(uint8_t i) const = 0;

        // ---- the emitter -----------------------------------------------------
        // Construct one into `mem` (emitterSize() bytes, 8-aligned); the
        // caller destroys it through the virtual destructor.
        virtual size_t   emitterSize() const = 0;
        virtual Emitter* makeEmitter(void* mem) const = 0;

        // ---- keys: what a key event is on this head unit ---------------------
        // One key frame: `code` with KEY_* (a knob code: a detent count). Its
        // length, 0 when it does not fit.
        virtual uint8_t keyFrame(uint8_t code, uint8_t state, uint8_t* out) const = 0;
        // The code an action sends when its keymap row names none; 0: none.
        virtual uint8_t keyCodeFor(Keys::Action a) const = 0;
        // A rotary code: one frame with a detent count, no release.
        virtual bool    isKnobCode(uint8_t code) const = 0;
        // Mute has no code of its own: it is a long press of
        // keyCodeFor(Mute) (RAV4: SOURCE held).
        virtual bool    muteIsLong() const = 0;
        virtual uint8_t     keyCodeCount() const = 0;
        virtual KeyCodeView keyCode(uint8_t i) const = 0;
        // "?" when the code is not in the list.
        const char* keyCodeName(uint8_t code) const;

        // ---- the decoder (inbound), made like the emitter ----------------------
        virtual size_t   decoderSize() const = 0;
        virtual Decoder* makeDecoder(void* mem) const = 0;
        // The byte that acknowledges a frame of the head unit's; -1: none is sent.
        virtual int16_t  ackByte() const = 0;

        // ---- framing -----------------------------------------------------------
        // One frame of the wire format around `cmd` and `payload` (the raw
        // console, macros): its length, 0 when it does not fit.
        virtual uint8_t frame(uint8_t cmd, const uint8_t* payload, uint8_t len, uint8_t* out) const = 0;
        // The other way, for the frame ring: a whole frame of ours into its
        // command and payload. False: not a frame (shown raw).
        virtual bool unframe(const uint8_t* f, uint8_t len, uint8_t& cmd, const uint8_t*& payload,
                             uint8_t& plen, uint16_t& total) const = 0;
        // For the frame ring: the track text a head-unit payload carries in a
        // form the protocol names itself (Raise: DUDU's 0x83 field), UTF-8,
        // and that field's id. False: none.
        virtual bool frameText(const uint8_t* p, uint8_t len, char* out, uint8_t cap, uint8_t& field) const
        {
            (void)p; (void)len; (void)out; (void)cap; (void)field;
            return false;
        }

        // ---- its settings (HuConfig::tuning) ----------------------------------
        // The defaults, into HuConfig::TUNING_MAX bytes at `t`.
        virtual void        initTuning(void* t) const = 0;
        // How the tuning is stored: NVS key and blob. fromBlob false = refused,
        // `t` unchanged.
        virtual const char* tuningKey() const = 0;
        virtual uint8_t     tuningBlobSize() const = 0;
        virtual void        tuningToBlob(const void* t, uint8_t* out) const = 0;
        virtual bool        tuningFromBlob(void* t, const uint8_t* in, uint8_t len) const = 0;
        // Its rows in the settings registry, its group first (raise, raise.*).
        // Their get/put read Cfg::Model::hu, the HuConfig::tuning bytes.
        virtual size_t             settingCount() const = 0;
        virtual const Cfg::SEntry& setting(size_t i) const = 0;

        // ---- generic, over the above -------------------------------------------
        // The field index for an id, or -1.
        int findField(const char* id) const;
        // The rule a field is sent by with the owner's map `own`: theirs,
        // else the default, else null (its own signal).
        const FieldRule* ruleFor(const FieldMap& own, uint8_t field) const;
        // defaults() with the owner's rules over them, into `out`: what is sent.
        void withDefaults(const FieldMap& own, FieldMap& out) const;
        // A HuConfig with this protocol's default tuning.
        void initConfig(HuConfig& c) const { c = HuConfig{}; initTuning(c.tuning); }
    };

    namespace Protocols
    {
        // Every protocol in this image, by id; the setting huProtocol picks one
        // at boot (index into this list).
        uint8_t         count();
        const Protocol& at(uint8_t i);
        int             find(const char* id);      // -1: not in this image
        // Once, at boot, before anything reads activeProtocol(). Out of range
        // = the first.
        void            select(uint8_t i);
    }

    // A field's integer with its decimal point placed: 4700, 2 -> "47.00".
    const char* fmtFixed(char* b, unsigned cap, int32_t v, uint8_t dec);
    // The other way, rounded: 47.004, 2 -> 4700. False when it does not fit.
    bool        toFixed(double v, uint8_t dec, int32_t& out);

    // The protocol this board speaks to the head unit.
    const Protocol& activeProtocol();
}
