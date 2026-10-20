#pragma once
#include "../Features.h"
#if FEATURE_USB_HID
#include "../keys/IKeySink.h"

// Keys -> the head unit as a WIRED keyboard: HID consumer control over the S3's
// own USB, plugged into the head unit's USB port. Same usages as the BLE
// HidKeySink, so Android treats both alike; nothing to pair, nothing to install.
//
// The device is composite (TinyUSB): the CDC console stays on the same cable.
namespace Sinks
{
    class UsbHidKeySink : public Keys::ISink {
    public:
        // The interface itself is registered by a static constructor, before the
        // core starts USB; this only readies the report path.
        void begin();
        // Sends the deferred release, or a press that waited for the host to
        // collect the release before it. The keys task, at nextDue().
        void service(uint32_t nowMs);
        // When that is due; false when nothing is pending.
        bool nextDue(uint32_t& dueMs) const;

        void setClasses(uint8_t m) { _mask = m; }
        uint8_t classes() const { return _mask; }
        void onKey(const Keys::Event& e) override;
        const char* id() const override { return "usb"; }

        uint32_t sent() const { return _sent; }
        uint32_t dropped() const { return _dropped; }   // host not ready / not plugged

    private:
        // Wired to the head unit, and the phone has AMS for transport: default to
        // both, unlike BLE HID's volume only.
        uint8_t  _mask = Keys::CLASS_TRANSPORT | Keys::CLASS_VOLUME;
        bool     _releasePending = false;
        uint32_t _pressedMs = 0;
        // A press behind a release the host has not collected yet.
        uint16_t _queued = 0;
        uint32_t _queuedMs = 0;
        // A report the host did not take: the next try, not the next tick,
        // so an unplugged host does not wake the keys task every millisecond.
        bool     _retry = false;
        uint32_t _retryMs = 0;
        uint32_t _sent = 0, _dropped = 0;
    };
}
#endif
