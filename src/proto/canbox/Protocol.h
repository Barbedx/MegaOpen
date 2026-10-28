#pragma once
#include <stdint.h>
#include "FieldMap.h"

// A head-unit protocol as the map sees it (docs/design/hu-mapping.md): its
// fields, each carried from a car signal, and the rules it starts with. The
// field map, /api/hu/fields, the catalog and the page read the active
// protocol through this and nothing else; Raise 112 (the RAV4 profile) is the
// one there is. ANOTHER PROTOCOL: a module that implements this (its field
// list, its defaults, its emitter) and activeProtocol() returning it.
namespace Canbox
{
    constexpr uint8_t NO_MSG = 0xFF;

    // One field with everything resolved. min, max and the values sent are
    // the signal's integers; `dec` places the decimal point (speed 4700,
    // dec 2 = 47.00 km/h). A field's id is the name of its signal.
    struct TargetView {
        const char* id;
        const char* group;
        const char* name;
        const char* unit;
        const char* cmd;      // "" when not sent
        const char* why;      // fw=false: why not, for the page; else null
        bool        boolean;
        int32_t     min, max;
        uint8_t     dec;
        bool        fw;       // false: a car value the protocol has no field for
        uint8_t     sig;
        uint8_t     msg;      // the protocol's message that carries it, NO_MSG none
    };

    class FieldProtocol {
    public:
        virtual ~FieldProtocol() = default;
        virtual const char* id() const = 0;            // "raise112"; the page's words are k.<words>.<field>
        virtual uint8_t     fieldCount() const = 0;
        virtual TargetView  field(uint8_t i) const = 0;
        // The rules a field starts with (FK_OFF for the ones no capture has
        // shown in use); the owner's map goes over them.
        virtual const FieldMap& defaults() const = 0;

        // The field index for an id, or -1.
        int findField(const char* id) const;
        // The rule a field is sent by with the owner's map `own`: theirs,
        // else the default, else null (its own signal).
        const FieldRule* ruleFor(const FieldMap& own, uint8_t field) const;
        // defaults() with the owner's rules over them, into `out`: what is sent.
        void withDefaults(const FieldMap& own, FieldMap& out) const;
    };

    // The protocol this build speaks to the head unit: the one place to swap it.
    const FieldProtocol& activeProtocol();
}
