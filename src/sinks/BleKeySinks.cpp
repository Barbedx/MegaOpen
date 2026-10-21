#include "BleKeySinks.h"
#if FEATURE_BLE
#include <Arduino.h>
#include "../apple_media_service.h"
#include "../bluetooth.h"
#include "../ble/HidRole.h"
#include "../utils/Log.h"

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
            const int16_t steps = _volStep.steps();
            const int16_t cap   = (int16_t)(VOL_PRESSES_MAX * steps);
            int16_t v = (int16_t)(_vol + (c.action == Action::VolUp ? steps : -steps));
            if (v >  cap) v =  cap;
            if (v < -cap) v = -cap;
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

    void BleKeyWorker::volBurstBegin(int8_t dir, uint32_t nowMs)
    {
        if (_burst.dir != dir) {
            // One before it not yet measured: its report may still land
            // after v0 is taken, so this one is not learned from.
            const bool overlap = _burst.dir != 0;
            _burst = Burst{};
            _burst.dir      = dir;
            _burst.v0       = AppleMediaService::Volume();
            _burst.reports0 = AppleMediaService::VolumeReports();
            _burst.startMs  = nowMs;
            _burst.clean    = !overlap;
        }
        _burst.endMs = 0;
    }

    // One line per press (or presses run together), once the phone's report
    // has had time to arrive: what it cost and how far it moved.
    void BleKeyWorker::volBurstSettle(uint32_t nowMs)
    {
        if (!_burst.dir || _vol || !_burst.endMs || (uint32_t)(nowMs - _burst.endMs) < VOL_SETTLE_MS) return;
        const float v1 = AppleMediaService::Volume();
        const bool updated = AppleMediaService::VolumeReports() != _burst.reports0;
        const bool learned = _burst.clean && _volStep.observe(_burst.v0, v1, _burst.writes, updated);
        LOGI("ble", "vol %c: %u write(s) %lu ms (%s), %.3f -> %.3f%s; %s %u/press",
             _burst.dir > 0 ? '+' : '-', (unsigned)_burst.writes,
             (unsigned long)(_burst.endMs - _burst.startMs),
             AppleMediaService::RemoteCommandNoResponse() ? "no-rsp" : "rsp",
             _burst.v0, v1, updated ? "" : " (no report)",
             learned ? "learned" : _volStep.learned() ? "kept" : "default",
             (unsigned)_volStep.steps());
        _burst = Burst{};
    }

    bool BleKeyWorker::step()
    {
        const uint32_t nowMs = millis();
        _hidHeld = false;             // only asked between a press and its release
        volBurstSettle(nowMs);
        if (!_fifoN && !_vol) return false;

        // No phone: what is pending is for a link that is not there.
        if (!Bluetooth::IsConnected()) {
            _noLink += _fifoN + (_vol ? 1u : 0u);
            _fifoN = 0;
            _vol = 0;
            _burst = Burst{};
            return false;
        }

        // A write with response paces itself (it waits for the answer); one
        // without goes out at once, so they are held a connection interval
        // apart, or a burst queues faster than the link drains it.
        if (AppleMediaService::RemoteCommandNoResponse() &&
            (uint32_t)(nowMs - _lastWriteMs) < Bluetooth::ConnIntervalMs())
            return true;

        // A refused write is counted and not retried (the link is going);
        // only the ones that went out count, in the burst too, or a
        // congested link would read as a smaller step.
        auto wrote = [&](bool ok) {
            _lastWriteMs = millis();
            if (ok) ++_amsWrites; else ++_amsFailed;
            return ok;
        };
        if (_fifoN) {
            const BleCmd c = _fifo[0];
            for (uint8_t i = 1; i < _fifoN; ++i) _fifo[i - 1] = _fifo[i];
            --_fifoN;
            // It may have waited behind a volume step; the rule is the same.
            if ((uint32_t)(nowMs - c.atMs) > STALE_MS) ++_stale;
            else wrote(amsCommand(c.action));
        } else {
            volBurstBegin(_vol > 0 ? 1 : -1, nowMs);
            bool ok;
            if (_vol > 0) { ok = AppleMediaService::VolumeUp();   --_vol; }
            else          { ok = AppleMediaService::VolumeDown(); ++_vol; }
            if (wrote(ok)) ++_burst.writes;
            if (!_vol) _burst.endMs = millis();
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
