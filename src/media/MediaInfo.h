#pragma once
#include <stdint.h>
#include <string>

// Source-neutral now-playing model (ARCHITECTURE-V2 §6.1): AMS or the head unit.
// No Arduino dependency, so it builds in the native test env.
struct MediaInfo {
    // Values mirror AMS's wire encoding so AmsMediaSource can static_cast; the HU
    // source maps its own states onto these.
    enum class PlaybackState : uint8_t {
        Paused = 0,
        Playing = 1,
        Rewinding = 2,
        FastForwarding = 3
    };
    enum class ShuffleMode : uint8_t { Off = 0, One = 1, All = 2 };
    enum class RepeatMode  : uint8_t { Off = 0, One = 1, All = 2 };

    std::string title;
    std::string artist;
    std::string album;
    std::string playerName;   // app/source name shown as the screen header

    PlaybackState playbackState = PlaybackState::Paused;
    float playbackRate = 0.0f;
    float elapsedTime  = 0.0f;   // seconds, as of lastUpdateMs
    float duration     = 0.0f;   // seconds, 0 = unknown
    float volume       = 0.0f;   // 0.0 .. 1.0

    int queueIndex = 0;
    int queueCount = 0;
    ShuffleMode shuffleMode = ShuffleMode::Off;
    RepeatMode  repeatMode  = RepeatMode::Off;

    // Clock ms when elapsedTime/playbackState were last set by the source
    // (0 = never). Lets consumers extrapolate elapsed without polling the source.
    uint32_t lastUpdateMs = 0;

    bool playing() const { return playbackState == PlaybackState::Playing; }

    // Elapsed seconds extrapolated to `nowMs` at playbackRate. Bounded, since a
    // phone that drops mid-song leaves "playing" set indefinitely.
    float elapsedAt(uint32_t nowMs) const {
        if (playbackState != PlaybackState::Playing || lastUpdateMs == 0)
            return elapsedTime;

        // lastUpdateMs slightly ahead of nowMs wraps to ~49 days; ignore it.
        const uint32_t dt = nowMs - lastUpdateMs;
        if (dt > 86400000u) return elapsedTime;

        float e = elapsedTime + (float)dt / 1000.0f * playbackRate;

        // Clamp to [0, duration]; playbackRate is signed.
        if (duration > 0.0f && e > duration) e = duration;
        if (e < 0.0f) e = 0.0f;
        return e;
    }
};
