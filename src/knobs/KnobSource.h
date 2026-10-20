#pragma once
#include "KnobConfig.h"
#include "../input/QuadDecoder.h"
#include "../input/PotStepper.h"

// Samples in, key events out. Portable (pins read elsewhere), native-tested.
// Also resolves the binding, so a web remap cannot pair a movement with the
// other table's meaning.
// Two encoders (ENC_RIGHT / ENC_LEFT), each with its own decoder and button.
namespace Knobs
{
    class Source {
    public:
        // srcId of every event made here. 0 matrix, 1 wheel, 2 web, 3 panel/macro.
        static constexpr uint8_t SRC_ID = 4;

        // `pair` = follow this Press with a Release. Not for wheel actions: the
        // sinks synthesise that (see CanboxUartSink).
        using EmitFn = void (*)(const Keys::Event& e, bool pair, void* ctx);

        void begin(EmitFn emit, void* ctx) { _emit = emit; _ctx = ctx; }
        void configure(const Config& c);
        // Forget the encoders' and buttons' history, as configure() does: after
        // an outage of the inputs (the expander came back), the next sample is
        // a position, not a movement, and a button already down is not a press
        // nor a hold.
        void reseed();

        // At ~1 kHz. Returns detents completed (for the lights).
        int8_t sampleEncoder(uint8_t k, bool a, bool b, uint32_t nowMs);

        // Same rate. True on the debounced press edge (for the lights).
        bool sampleButton(uint8_t k, bool down, uint32_t nowMs);

        // One ADC reading (50 Hz is plenty). Returns zones crossed; emits them as
        // key events only in PotMode::Steps.
        int8_t samplePot(uint16_t raw, uint32_t nowMs);

        const Input::QuadDecoder& encoder(uint8_t k = ENC_RIGHT) const { return _quad[k < ENCODERS ? k : 0]; }
        const Input::PotStepper&  pot() const { return _pot; }
        bool     buttonDown(uint8_t k = ENC_RIGHT) const { return _btn[k < ENCODERS ? k : 0].stable; }
        uint32_t clicks(uint8_t k = ENC_RIGHT) const { return _btn[k < ENCODERS ? k : 0].clicks; }
        uint32_t holds(uint8_t k = ENC_RIGHT)  const { return _btn[k < ENCODERS ? k : 0].holds; }
        uint32_t potSteps() const { return _potSteps; }

    private:
        static constexpr uint16_t DEBOUNCE_MS = 20;
        // Caps a full pot sweep (~60 steps) per sample.
        static constexpr int8_t   MAX_POT_BURST = 8;

        struct Button {
            bool     stable = false, cand = false, seeded = false;
            uint32_t sinceMs = 0, downMs = 0;
            bool     holdFired = false;
            uint32_t clicks = 0, holds = 0;
        };

        void fire(const Binding& b, uint32_t nowMs);

        EmitFn _emit = nullptr;
        void*  _ctx = nullptr;
        Config _c;

        Input::QuadDecoder _quad[ENCODERS];
        Button             _btn[ENCODERS];
        Input::PotStepper  _pot;
        uint32_t           _potSteps = 0;
    };
}
