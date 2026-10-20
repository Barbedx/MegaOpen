#pragma once
#include <stdint.h>

// The logical key vocabulary (ARCHITECTURE.md §5). Nothing in the core knows a
// head unit's codes: sinks translate KeyAction -> their own protocol, so a new
// head unit is a new profile file, not a change to the scanner or the router.
namespace Keys
{
    enum class Action : uint8_t {
        None = 0,
        Load,        // the button at the bottom of the OEM remote
        SrcNext,
        SrcPrev,
        VolUp,
        VolDown,
        Mute,
        EncCw,       // encoder detent, clockwise  (Megane: seek up)
        EncCcw,      //                counter-cw  (Megane: seek down)
        Ok,
        Phone,
        Voice,
        Mode,        // the Megane keypad's MODE button
        Radio,       // the Megane keypad's RADIO/preset button
        // Wheel contacts; matrix positions only, EncoderDecoder makes EncCw/EncCcw.
        EncPhaseA,
        EncPhaseB,
        EncPhaseC,
        _Count
    };

    enum class State : uint8_t {
        Release = 0,
        Press   = 1,
        Long    = 2,   // held past the long-press threshold
        Repeat  = 3,   // auto-repeat while held (volume, seek)
    };

    // One event as it travels through the core. srcId identifies the producer
    // (0 = matrix, 1 = encoder, 2 = web/test injection, 3 = OEM panel or macro,
    // 4 = the front-panel knobs) for diagnostics.
    struct Event {
        Action   action = Action::None;
        State    state  = State::Release;
        uint32_t tMs    = 0;
        uint8_t  srcId  = 0;
        // Per-position code override from KeyMap, resolved where the position is
        // still known. 0 means "sink, use your profile's default for the action".
        uint8_t  codeHint = 0;
    };

    // ---- routing classes ---------------------------------------------------
    // Sinks subscribe to classes, not keys, so routing survives remapping.
    // Classes follow device ownership: transport -> the player (phone), volume ->
    // the head unit (amp), phone/voice -> handset, UI -> head-unit screen.
    // Here rather than in a sink header: the UART sink is host-tested, no BLE includes.
    enum : uint8_t {
        CLASS_NONE      = 0x00,
        CLASS_TRANSPORT = 0x01,   // play/pause, next/prev, source, wheel detents
        CLASS_VOLUME    = 0x02,   // up, down, mute
        CLASS_PHONE     = 0x04,   // answer/hang up, voice assistant
        CLASS_UI        = 0x08,   // MODE, RADIO — the head unit's own screen
        CLASS_ALL       = 0x0F,
    };
    static constexpr uint8_t CLASS_BITS = 4;

    // Encoder phases are CLASS_NONE: matrix positions, never events.
    uint8_t classOf(Action a);
    const char* classTitle(uint8_t bit);     // single bit -> its name, for /api/catalog

    inline constexpr bool isVolume(Action a) {
        return a == Action::VolUp || a == Action::VolDown;
    }
    inline constexpr bool isEncoder(Action a) {
        return a == Action::EncCw || a == Action::EncCcw;
    }
    inline constexpr bool isEncoderPhase(Action a) {
        return a == Action::EncPhaseA || a == Action::EncPhaseB || a == Action::EncPhaseC;
    }

    const char* name(Action a);        // for the web UI and the log
    const char* title(Action a);       // Ukrainian, for /api/catalog
    Action      fromName(const char* s);
}
