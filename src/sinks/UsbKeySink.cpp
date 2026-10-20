#include "UsbKeySink.h"
#if FEATURE_USB_HID
#include <Arduino.h>
#include <USB.h>
#include <USBHIDConsumerControl.h>
#include "esp32-hal-tinyusb.h"     // tud_hid_n_report: queue and return

namespace Sinks
{
    using Keys::Action;
    using Keys::State;

    // Constructed before setup(): its constructor registers the HID interface
    // with TinyUSB, which must happen before USB.begin().
    static USBHIDConsumerControl g_cc;
    // ready() lives on USBHID, which is only a handle on TinyUSB's one HID.
    static USBHID g_hid;

    // The press is held this long before the zero report, as on BLE; some head
    // units ignore a key whose release arrives in the same poll.
    static constexpr uint32_t KEY_HOLD_MS = 30;
    // A host that has not collected a report in this long is gone (unplugged):
    // the waiting press is dropped, the waiting release given up.
    static constexpr uint32_t GIVE_UP_MS = 200;
    // Between tries while the host is not taking reports.
    static constexpr uint32_t RETRY_MS = 5;

    // Queued with TinyUSB, and back at once. USBHIDConsumerControl::press()
    // would also wait for the host to collect the report — up to 100 ms with
    // no host — and this runs on the keys task. False when not ready:
    // unplugged, unconfigured, or the last report not collected yet.
    static bool sendUsage(uint16_t v)
    {
        if (!g_hid.ready()) return false;
        return tud_hid_n_report(0, HID_REPORT_ID_CONSUMER_CONTROL, &v, sizeof(v));
    }

    void UsbHidKeySink::begin()
    {
        g_cc.begin();
        USB.begin();   // no-op: with the CDC console on boot the core started it
    }

    static uint16_t usageFor(Action a)
    {
        switch (a) {
            case Action::Ok:      return CONSUMER_CONTROL_PLAY_PAUSE;
            case Action::Load:    return CONSUMER_CONTROL_PLAY_PAUSE;
            case Action::EncCw:   return CONSUMER_CONTROL_SCAN_NEXT;
            case Action::EncCcw:  return CONSUMER_CONTROL_SCAN_PREVIOUS;
            case Action::SrcNext: return CONSUMER_CONTROL_SCAN_NEXT;
            case Action::SrcPrev: return CONSUMER_CONTROL_SCAN_PREVIOUS;
            case Action::VolUp:   return CONSUMER_CONTROL_VOLUME_INCREMENT;
            case Action::VolDown: return CONSUMER_CONTROL_VOLUME_DECREMENT;
            case Action::Mute:    return CONSUMER_CONTROL_MUTE;
            default:              return 0;
        }
    }

    void UsbHidKeySink::onKey(const Keys::Event& e)
    {
        if (e.state != State::Press && e.state != State::Repeat) return;
        const uint8_t c = Keys::classOf(e.action);
        if (c == Keys::CLASS_NONE || (_mask & c) == 0) return;
        const uint16_t u = usageFor(e.action);
        if (!u) return;

        if (_releasePending) {
            // Never merge two presses: the release goes now, and this press
            // once the host has collected it (service()). A release the host
            // cannot take yet is sent by service() first; the press still waits.
            if (sendUsage(0)) _releasePending = false;
            if (_queued) ++_dropped;
            _queued = u;
            _queuedMs = millis();
            return;
        }
        if (sendUsage(u)) { ++_sent; _releasePending = true; _pressedMs = millis(); _retry = false; }
        else              ++_dropped;
    }

    bool UsbHidKeySink::nextDue(uint32_t& dueMs) const
    {
        // A waiting press: as soon as the host is ready (the keys task waits a
        // tick at least), or at the retry time once the host has refused.
        uint32_t due;
        if (_queued)              due = _queuedMs;
        else if (_releasePending) due = _pressedMs + KEY_HOLD_MS;
        else                      return false;
        if (_retry && (int32_t)(_retryMs - due) > 0) due = _retryMs;
        dueMs = due;
        return true;
    }

    void UsbHidKeySink::service(uint32_t nowMs)
    {
        if (!_queued && !_releasePending) { _retry = false; return; }
        if (_retry && (int32_t)(nowMs - _retryMs) < 0) return;
        _retry = false;

        if (_releasePending && (_queued || (uint32_t)(nowMs - _pressedMs) >= KEY_HOLD_MS)) {
            // The release first, early when a press waits behind it; that
            // press goes at a later pass, once the host has collected this.
            if (sendUsage(0)) { _releasePending = false; return; }
            if ((uint32_t)(nowMs - _pressedMs) >= KEY_HOLD_MS + GIVE_UP_MS) {
                // A host gone that long took the key state with it.
                _releasePending = false;
            } else {
                _retry = true;
                _retryMs = nowMs + RETRY_MS;
            }
            if (_queued && (uint32_t)(nowMs - _queuedMs) >= GIVE_UP_MS) { ++_dropped; _queued = 0; }
            return;
        }
        if (!_queued) return;
        if (sendUsage(_queued)) {
            ++_sent;
            _releasePending = true;
            _pressedMs = nowMs;
            _queued = 0;
        } else if ((uint32_t)(nowMs - _queuedMs) >= GIVE_UP_MS) {
            ++_dropped;
            _queued = 0;
        } else {
            _retry = true;
            _retryMs = nowMs + RETRY_MS;
        }
    }
}
#endif
