#pragma once
#include <stdint.h>
#include "../../vehicle/VehicleState.h"
#include "FieldMap.h"

// What a head-unit protocol's emitter reads (docs/design/hu-mapping.md): its
// fields, by field id, each taken from the car's State through the owner's
// field map (FieldMap.h). A field with no rule is the signal of its own id, so
// with an empty map the bytes on the wire are exactly what they were when the
// emitter read State itself.
//
// A State converts to a FieldView on its own (no map), so every caller and
// test that hands the emitter a State keeps working; the emitter puts its own
// map on at its entry points.
namespace Canbox
{
    class FieldView
    {
    public:
        FieldView(const Vehicle::State& s, const FieldMap* m = nullptr) : _s(s), _m(m) {}   // NOLINT: implicit on purpose

        bool has(uint8_t field) const
        {
            const FieldRule* r = rule(field);
            if (!r) return _s.has(field);
            if (r->kind == FK_OFF) return false;
            if (r->kind == FK_CONST) return true;
            return _s.has(r->src);
        }
        int32_t get(uint8_t field, int32_t fallback = 0) const
        {
            const FieldRule* r = rule(field);
            if (!r) return _s.get(field, fallback);
            if (r->kind == FK_OFF) return fallback;
            if (r->kind == FK_CONST) return Vehicle::State::clamp(field, r->add);
            if (!_s.has(r->src)) return fallback;
            return Vehicle::State::clamp(field, FieldMap::scale(*r, _s.get(r->src)));
        }
        // Always fresh: a value typed on the page, or a constant.
        bool pinned(uint8_t field) const
        {
            const FieldRule* r = rule(field);
            if (!r) return _s.pinned(field);
            if (r->kind == FK_CONST) return true;
            return r->kind == FK_SIGNAL && _s.pinned(r->src);
        }

        const Vehicle::State& state() const { return _s; }
        const FieldMap*       map() const   { return _m; }

    private:
        const FieldRule* rule(uint8_t field) const { return _m ? _m->find(field) : nullptr; }

        const Vehicle::State& _s;
        const FieldMap*       _m;
    };
}
