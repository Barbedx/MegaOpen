#include "Protocol.h"
#include <string.h>
#include "RaiseTargets.h"
#include "Rav4Emitter.h"

namespace Canbox
{
    int FieldProtocol::findField(const char* id) const
    {
        if (!id) return -1;
        for (uint8_t i = 0; i < fieldCount(); ++i)
            if (!strcmp(field(i).id, id)) return i;
        return -1;
    }

    const FieldRule* FieldProtocol::ruleFor(const FieldMap& own, uint8_t f) const
    {
        const FieldRule* r = own.find(f);
        return r ? r : defaults().find(f);
    }

    void FieldProtocol::withDefaults(const FieldMap& own, FieldMap& out) const
    {
        out = defaults();
        for (uint8_t i = 0; i < own.count(); ++i) out.set(own.at(i));
    }

    namespace
    {
        // Raise protocol 112, the RAV4 profile: RaiseTargets' rows, the
        // emitter's defaults.
        struct Raise112 final : FieldProtocol {
            const char* id() const override { return "raise112"; }
            uint8_t     fieldCount() const override { return RaiseTargets::count(); }
            TargetView  field(uint8_t i) const override { return RaiseTargets::at(i); }
            const FieldMap& defaults() const override { return Rav4Emitter::defaults(); }
        };
    }

    const FieldProtocol& activeProtocol()
    {
        static const Raise112 p;
        return p;
    }
}
