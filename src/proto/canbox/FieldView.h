#pragma once
#include <stdint.h>
#include "../../vehicle/VehicleState.h"

// What a head-unit protocol's emitter reads (docs/design/hu-mapping.md): its
// fields, by field id, each taken from the car's State through the field map.
// Step 1: the map is the identity (a field is the signal of the same id), so
// the bytes on the wire are exactly what they were when the emitter read State
// itself. Step 2 adds the owner's map: a field from another signal, a
// constant, or off, with a scale.
//
// A State converts to a FieldView on its own (the identity), so every caller
// and test that hands the emitter a State keeps working unchanged.
namespace Canbox
{
    class FieldView
    {
    public:
        FieldView(const Vehicle::State& s) : _s(s) {}   // NOLINT: implicit on purpose

        bool    has(uint8_t field) const { return _s.has(field); }
        int32_t get(uint8_t field, int32_t fallback = 0) const { return _s.get(field, fallback); }
        // A value typed on the page (always fresh), as State has it.
        bool    pinned(uint8_t field) const { return _s.pinned(field); }

        const Vehicle::State& state() const { return _s; }

    private:
        const Vehicle::State& _s;
    };
}
