#pragma once
#include "IMediaSource.h"

// The iPhone (AMS) as a media source; converts AMS to MediaInfo so nothing
// downstream sees AMS types. Target-only, excluded from the native env.
namespace AppleMediaService { struct MediaInformation; }

class AmsMediaSource : public IMediaSource {
public:
    // Call only when the AMS role is active (BleHub mode ams/both).
    void begin();

    const MediaInfo& current() const override { return _info; }
    bool active() const override;         // Bluetooth::IsConnected()
    const char* statusText() const override;
    const char* name() const override { return "ams"; }

    // Cross-task-safe copy. _info is written on the NimBLE host task; other
    // tasks must use this, not current() (std::string fields aren't atomic).
    MediaInfo snapshot() const override;
    bool trySnapshot(MediaInfo& out, uint32_t waitMs) const override;

private:
    void onAmsUpdate(const AppleMediaService::MediaInformation& mi);
    MediaInfo _info;
    mutable void* _mutex = nullptr;   // SemaphoreHandle_t (created in begin)
};
