#include "BleKeySinks.h"
#if FEATURE_BLE
#include <Arduino.h>
#include "../apple_media_service.h"
#include "../bluetooth.h"
#include "../ble/HidRole.h"

// Not in the ported HidRole.h; kept here so that header stays a clean copy.
#ifndef KEY_MEDIA_MUTE
#define KEY_MEDIA_MUTE 0x00E2
#endif

namespace Sinks
{
    using Keys::Action;

    static uint16_t hidUsage(Action a)
    {
        switch (a) {
            case Action::Ok:      return KEY_MEDIA_PLAY_PAUSE;
            case Action::EncCw:   return KEY_MEDIA_NEXT_TRACK;
            case Action::EncCcw:  return KEY_MEDIA_PREVIOUS_TRACK;
            case Action::SrcNext: return KEY_MEDIA_NEXT_TRACK;
            case Action::SrcPrev: return KEY_MEDIA_PREVIOUS_TRACK;
            case Action::VolUp:   return KEY_MEDIA_VOLUME_UP;
            case Action::VolDown: return KEY_MEDIA_VOLUME_DOWN;
            case Action::Mute:    return KEY_MEDIA_MUTE;
            default:              return 0;
        }
    }

    // The AMS command for a non-volume action, or false for none.
    static bool amsCommand(Action a)
    {
        switch (a) {
            case Action::Ok:
            case Action::Load:    return AppleMediaService::Toggle();
            case Action::EncCw:
            case Action::SrcNext: return AppleMediaService::NextTrack();
            case Action::EncCcw:
            case Action::SrcPrev: return AppleMediaService::PrevTrack();
            default:              return false;
        }
    }

    static bool amsHasCommand(Action a)
    {
        switch (a) {
            case Action::Ok: case Action::Load:
            case Action::EncCw: case Action::SrcNext:
            case Action::EncCcw: case Action::SrcPrev:
                return true;
            default:
                return false;
        }
    }

    void BleKeyWorker::accept(const BleCmd& c, uint32_t nowMs)
    {
        if ((uint32_t)(nowMs - c.atMs) > STALE_MS) { ++_stale; return; }

        if (c.target == BleCmd::Target::Hid) {
            // Hid::press no-ops with no subscribed peer, so no connection gate.
            const uint16_t u = hidUsage(c.action);
            if (u && Hid::press(u)) {
                ++_hidPresses;
                _hidHeld = true;
                _hidPressAtMs = millis();
            }
            return;
        }

        if (Keys::isVolume(c.action)) {
            if (_vol) ++_coalesced;
            int16_t v = (int16_t)(_vol + (c.action == Action::VolUp ? VOL_STEPS : -VOL_STEPS));
            if (v >  VOL_PENDING_MAX) v =  VOL_PENDING_MAX;
            if (v < -VOL_PENDING_MAX) v = -VOL_PENDING_MAX;
            _vol = v;
            return;
        }
        if (!amsHasCommand(c.action)) return;
        if (_fifoN >= FIFO_MAX) { ++_stale; return; }
        _fifo[_fifoN++] = c;
    }

    bool BleKeyWorker::hidHoldLeft(uint32_t nowMs, uint32_t& leftMs) const
    {
        if (!_hidHeld) return false;
        const uint32_t held = nowMs - _hidPressAtMs;
        if (held >= HID_HOLD_MS) return false;
        leftMs = HID_HOLD_MS - held;
        return true;
    }

    bool BleKeyWorker::step()
    {
        const uint32_t nowMs = millis();
        _hidHeld = false;             // only asked between a press and its release
        if (!_fifoN && !_vol) return false;

        // No phone: what is pending is for a link that is not there.
        if (!Bluetooth::IsConnected()) {
            _noLink += _fifoN + (_vol ? 1u : 0u);
            _fifoN = 0;
            _vol = 0;
            return false;
        }

        if (_fifoN) {
            const BleCmd c = _fifo[0];
            for (uint8_t i = 1; i < _fifoN; ++i) _fifo[i - 1] = _fifo[i];
            --_fifoN;
            // It may have waited behind a volume step; the rule is the same.
            if ((uint32_t)(nowMs - c.atMs) > STALE_MS) ++_stale;
            else { amsCommand(c.action); ++_amsWrites; }
        } else {
            if (_vol > 0) { AppleMediaService::VolumeUp();   --_vol; }
            else          { AppleMediaService::VolumeDown(); ++_vol; }
            ++_amsWrites;
        }
        return _fifoN || _vol;
    }

    void BleKeyWorker::collect(uint32_t waitMs)
    {
        BleCmd c;
        if (!BleKeyQueue::take(c, waitMs)) return;
        accept(c, millis());
        while (BleKeyQueue::take(c, 0)) accept(c, millis());
    }
}
#endif
