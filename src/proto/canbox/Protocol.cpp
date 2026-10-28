#include "Protocol.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "Raise112.h"

namespace Canbox
{
    int Protocol::findField(const char* id) const
    {
        if (!id) return -1;
        for (uint8_t i = 0; i < fieldCount(); ++i)
            if (!strcmp(field(i).id, id)) return i;
        return -1;
    }

    const FieldRule* Protocol::ruleFor(const FieldMap& own, uint8_t f) const
    {
        const FieldRule* r = own.find(f);
        return r ? r : defaults().find(f);
    }

    void Protocol::withDefaults(const FieldMap& own, FieldMap& out) const
    {
        out = defaults();
        for (uint8_t i = 0; i < own.count(); ++i) out.set(own.at(i));
    }

    const char* Protocol::keyCodeName(uint8_t code) const
    {
        for (uint8_t i = 0; i < keyCodeCount(); ++i) {
            const KeyCodeView k = keyCode(i);
            if (k.code == code) return k.name;
        }
        return "?";
    }

    const char* fmtFixed(char* b, unsigned cap, int32_t v, uint8_t dec)
    {
        if (!dec) { snprintf(b, cap, "%ld", (long)v); return b; }
        int32_t p = 1;
        for (uint8_t i = 0; i < dec; ++i) p *= 10;
        const int64_t a = v < 0 ? -(int64_t)v : v;
        snprintf(b, cap, "%s%ld.%0*ld", v < 0 ? "-" : "", (long)(a / p), (int)dec, (long)(a % p));
        return b;
    }

    bool toFixed(double v, uint8_t dec, int32_t& out)
    {
        for (uint8_t i = 0; i < dec; ++i) v *= 10;
        if (!(v > -2147483648.0 && v < 2147483647.0)) return false;   // NaN too
        out = (int32_t)lround(v);
        return true;
    }

    namespace Protocols
    {
        namespace
        {
            const Protocol* const kAll[] = {&raise112()};
            constexpr uint8_t N = sizeof(kAll) / sizeof(kAll[0]);
            uint8_t s_active = 0;
        }

        uint8_t         count() { return N; }
        const Protocol& at(uint8_t i) { return *kAll[i < N ? i : 0]; }

        int find(const char* id)
        {
            if (!id) return -1;
            for (uint8_t i = 0; i < N; ++i)
                if (!strcmp(kAll[i]->id(), id)) return i;
            return -1;
        }

        void select(uint8_t i) { s_active = i < N ? i : 0; }
    }

    const Protocol& activeProtocol() { return Protocols::at(Protocols::s_active); }
}
