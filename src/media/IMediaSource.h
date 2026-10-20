#pragma once
#include "MediaInfo.h"

// A provider of now-playing state (ARCHITECTURE-V2 §6.1). Implementations:
// AmsMediaSource (iPhone over BLE AMS), HuLinkMediaSource (DUDU7 head unit over
// LinkProto, P1+). Portable interface — function-pointer callback, no <functional>.
class IMediaSource {
public:
    using ChangeCb = void (*)(IMediaSource& source, void* ctx);

    virtual ~IMediaSource() = default;

    virtual void poll() {}                          // per-loop pump (optional)

    // Only safe on the writing task (AMS writes from the NimBLE host task).
    virtual const MediaInfo& current() const = 0;

    // Safe from any task: sources written from another task override this
    // with a locked copy.
    virtual MediaInfo snapshot() const { return current(); }

    // The same copy, but waiting at most waitMs for the source's lock. False =
    // busy, `out` untouched: the caller is a task that must not stall (the
    // panel's), and skips this pass instead.
    virtual bool trySnapshot(MediaInfo& out, uint32_t waitMs) const
    {
        (void)waitMs;
        out = current();
        return true;
    }

    virtual bool active() const = 0;                // link up / source usable now
    virtual const char* statusText() const { return ""; }  // short line for the display
    virtual const char* name() const = 0;           // "ams" | "hu"

    void setChangeCallback(ChangeCb cb, void* ctx) { _cb = cb; _ctx = ctx; }

protected:
    void notifyChange() { if (_cb) _cb(*this, _ctx); }

private:
    ChangeCb _cb = nullptr;
    void*    _ctx = nullptr;
};
