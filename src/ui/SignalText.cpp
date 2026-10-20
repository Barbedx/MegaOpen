#include "SignalText.h"
#include <stdio.h>
#include <string.h>

namespace MenuUi
{
    void signalValue(char* out, size_t cap, const Vehicle::Meta& m, int32_t v, bool fresh, bool verified)
    {
        if (!cap) return;
        if (!fresh) {
            snprintf(out, cap, "--");
        } else if (m.boolean) {
            snprintf(out, cap, "%s", v ? "on" : "off");
        } else if (m.decimals) {
            int32_t div = 1;
            for (uint8_t i = 0; i < m.decimals; ++i) div *= 10;
            const int32_t a = v < 0 ? -v : v;
            snprintf(out, cap, "%s%ld.%0*ld%s", v < 0 ? "-" : "", (long)(a / div),
                     (int)m.decimals, (long)(a % div), m.unit);
        } else {
            snprintf(out, cap, "%ld%s", (long)v, m.unit);
        }
        if (!verified) {
            const size_t n = strlen(out);
            if (n + 1 < cap) { out[n] = '?'; out[n + 1] = 0; }
        }
    }

    void signalRow(char* out, size_t cap, size_t width, const Vehicle::Meta& m, int32_t v,
                   bool fresh, bool verified)
    {
        if (!cap) return;
        char val[24];
        signalValue(val, sizeof(val), m, v, fresh, verified);
        const size_t w = cap - 1 < width ? cap - 1 : width;
        const size_t vl = strlen(val) < w ? strlen(val) : w;
        size_t nl = strlen(m.name);
        if (nl + vl + 1 > w) nl = w > vl + 1 ? w - vl - 1 : 0;
        memset(out, ' ', w);
        memcpy(out, m.name, nl);
        memcpy(out + (w - vl), val, vl);
        out[w] = 0;
    }
}
