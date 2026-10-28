#pragma once
#include <stdint.h>
#include "Rav4Emitter.h"
#include "Protocol.h"   // TargetView, NO_MSG

// Every field the RAV4 profile can carry to the head unit, one row each
// (docs/design/web-ui.md, slice 7; docs/design/event-tasks.md §5b, the mapping
// page). /api/catalog serves it as `raise`, /api/map binds each row to the
// car, and /api/map/test sends one row's message with a typed value.
//
// A row names its Vehicle::Sig and the Rav4Emitter message that carries it;
// name, unit, range and decimals come from the signal's own metadata
// (Signals.cpp), so only the wire command is here; the page has the words (k.raise.*).
// ADDING A TOYOTA FUNCTION: a Sig, a builder in Rav4Emitter, and one row here.
//
// A row with fw=false is a car value protocol 112 has no field for (the
// battery): listed with its signal so the page shows the value and `why`, and
// points at the experiment that borrows another field for it.
namespace Canbox
{
    namespace RaiseTargets
    {
        uint8_t    count();
        TargetView at(uint8_t i);
        // The row index for an id, or -1.
        int        find(const char* id);

        // A signal's integer with its decimal point placed: 4700, 2 -> "47.00".
        const char* fmt(char* b, unsigned cap, int32_t v, uint8_t dec);
        // The other way, rounded: 47.004, 2 -> 4700. False when it does not fit.
        bool        toInt(double v, uint8_t dec, int32_t& out);
    }
}
