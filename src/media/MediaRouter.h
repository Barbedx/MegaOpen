#pragma once
#include "IMediaSource.h"

// Picks the active media source (ARCHITECTURE-V2 §6.1). Config media_source:
// "ams" | "hu" | "auto" (auto: the HU wins while its link is up and sending,
// else AMS). Portable — native tests drive it with fake sources.
class MediaRouter {
public:
    enum class Mode : uint8_t { Ams, Hu, Auto };

    void setSources(IMediaSource* ams, IMediaSource* hu) { _ams = ams; _hu = hu; }
    void setMode(Mode m) { _mode = m; }
    Mode mode() const { return _mode; }
    static Mode modeFromStr(const char* s);
    static const char* modeStr(Mode m);

    // /api/catalog mediaSrc: every value of the media_source setting (pick,
    // with its Mode) and of sourceName() (all rows). `ams`: needs a BLE mode
    // that runs AMS.
    struct SourceRow { const char* id; const char* name; bool pick; bool ams; Mode mode; };
    static constexpr uint8_t SOURCE_ROWS = 4;
    static const SourceRow SOURCES[SOURCE_ROWS];

    // The source the current mode selects (may be inactive). Null only if that
    // slot was never registered.
    IMediaSource* activeSource() const;

    const MediaInfo& current() const;     // active source's info (empty if none)
    // Cross-task-safe read. Use it anywhere but the source's own writer task
    // (for AMS: everywhere except the AMS notification callback).
    MediaInfo snapshot() const;
    // The same, waiting at most waitMs for the source's lock (IMediaSource).
    bool trySnapshot(MediaInfo& out, uint32_t waitMs) const;
    bool active() const;                  // active source exists and is live
    const char* statusText() const;
    const char* sourceName() const;

private:
    IMediaSource* _ams = nullptr;
    IMediaSource* _hu = nullptr;
    Mode _mode = Mode::Ams;
};
