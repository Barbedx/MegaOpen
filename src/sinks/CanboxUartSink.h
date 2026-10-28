#pragma once
#include "../keys/IKeySink.h"
#include "../proto/canbox/Protocol.h"

// Key events -> the head-unit protocol's key frames on the UART (Raise: 0x20;
// ARCHITECTURE.md §6.1). What a key is on the wire (its code, knob or button,
// mute as a long press) is the protocol's; the timing here is every
// protocol's. Portable (write callback + tick()) for native tests.
//
// A detent has no release of its own, so a non-knob wheel step is a press now
// and a scheduled (not delayed) release: _pulseMs after the detent happened,
// but never sooner than MIN_GAP_MS after the press left (Keys::releaseDue).
// A key whose release arrives right behind its press (a Tap, a panel key, a
// macro) gets the same rule: the release is scheduled MIN_GAP_MS after the
// press instead of following it 1.5 ms later on the wire.
namespace Sinks
{
    class CanboxUartSink : public Keys::ISink {
    public:
        using WriteFn = void (*)(const uint8_t* frame, uint8_t len, void* ctx);
        // The same, saying whether the frame was taken (UartTx::writeKey). A
        // scheduled release that was not is kept and tried again at tick().
        using WriteOkFn = bool (*)(const uint8_t* frame, uint8_t len, void* ctx);

        void begin(WriteFn write, void* ctx)   { _write = write; _writeOk = nullptr; _ctx = ctx; }
        void begin(WriteOkFn write, void* ctx) { _writeOk = write; _write = nullptr; _ctx = ctx; }

        // millis() on the board. Needed for the release rule: a detent that
        // waited in the KeyBus must still get MIN_GAP_MS before its release.
        // Without a clock the event's own tMs stands in for "now".
        using NowFn = uint32_t (*)();
        void setClock(NowFn now) { _now = now; }

        // `enabled` gates the whole link, sendKey() included. `classes` routes
        // only key events; clearing it leaves telemetry flowing. Switching the
        // link off first sends every scheduled release, so no key stays down.
        void setEnabled(bool on);
        bool enabled() const { return _enabled; }
        void setClasses(uint8_t m) { _mask = m; }
        uint8_t classes() const { return _mask; }
        void setPulseMs(uint16_t ms) { _pulseMs = ms; }
        // The protocol whose key frames go out; unset = activeProtocol().
        void setProtocol(const Canbox::Protocol& p) { _proto = &p; }

        void onKey(const Keys::Event& e) override;
        const char* id() const override { return "uart"; }

        // ---- what this event would put on the wire ----
        // onKey() and the panel's key popup share this one resolver, so the
        // popup cannot name a byte the sink did not send.
        struct Explain {
            bool     sends   = false;  // a frame leaves for this event
            uint8_t  code    = 0;
            uint8_t  state   = 0;      // Canbox::KEY_* — or a detent count when knob
            bool     knob    = false;  // second byte is a count, not a state
            bool     release = false;  // ...and a release will follow, scheduled
            // Short literal for a 26-column row, when sends is false.
            const char* why  = "";
        };
        Explain explain(const Keys::Event& e) const;

        // Retire scheduled releases. The keys task calls it at nextDue().
        void tick(uint32_t nowMs);
        // The earliest scheduled release; false when none is pending.
        bool nextDue(uint32_t& dueMs) const;

        // Arbitrary code/state pair (web test button, vehicle emitter). False
        // when nothing was written (link off, or the write was refused).
        bool sendKey(uint8_t code, uint8_t state);

        // `state` now and, unless it is a knob code (a detent count) or a
        // release, the release scheduled MIN_GAP_MS later rather than on the
        // heels of the press (IKeySink.h). For the web literal and the KeyCode
        // macro.
        void sendTap(uint8_t code, uint8_t state);

        uint32_t framesSent() const { return _framesSent; }

    private:
        static constexpr uint8_t MAX_PENDING = 8;
        // A scheduled release whose write keeps failing is given up after
        // this long past due, so a dead link cannot wake the keys task every
        // tick for ever.
        static constexpr uint32_t RETRY_MAX_MS = 1000;
        struct Pending { uint8_t code; uint32_t dueMs; bool used; };

        uint32_t nowOr(uint32_t tMs) const { return _now ? _now() : tMs; }
        void schedule(uint8_t code, uint32_t dueMs);
        // A press of `code` while its release is still scheduled: that
        // release goes first, so presses and releases keep alternating.
        void releasePendingOf(uint8_t code);
        // Sends a non-release frame, remembering when a press of `code` left.
        bool sendPress(uint8_t code, uint8_t state, uint32_t nowMs);

        const Canbox::Protocol& proto() const { return _proto ? *_proto : Canbox::activeProtocol(); }
        // Mute sent as its code held (Protocol::muteIsLong), not as a code of its own.
        bool longMute(const Keys::Event& e) const;

        const Canbox::Protocol* _proto = nullptr;
        WriteFn   _write = nullptr;
        WriteOkFn _writeOk = nullptr;
        void*     _ctx = nullptr;
        NowFn     _now = nullptr;
        bool      _enabled = true;
        uint8_t   _mask = Keys::CLASS_ALL;
        uint16_t  _pulseMs = 60;
        uint32_t  _framesSent = 0;
        Pending   _pending[MAX_PENDING] = {};
        // The last press that left, for the release gap (MIN_GAP_MS).
        uint8_t   _lastPressCode = 0;
        uint32_t  _lastPressMs = 0;
        // Mute is SOURCE held: a Long after the Press that already sent it
        // would hold it a second time.
        bool      _muteDown = false;
    };
}
