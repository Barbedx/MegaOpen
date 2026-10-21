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

    // name left, value flush right, on exactly `width` columns (cut to cap-1);
    // the value wins over the name.
    static void lay(char* out, size_t cap, size_t width, const char* name, const char* val)
    {
        const size_t w = cap - 1 < width ? cap - 1 : width;
        const size_t vl = strlen(val) < w ? strlen(val) : w;
        size_t nl = strlen(name);
        if (nl + vl + 1 > w) nl = w > vl + 1 ? w - vl - 1 : 0;
        memset(out, ' ', w);
        memcpy(out, name, nl);
        memcpy(out + (w - vl), val, vl);
        out[w] = 0;
    }

    void signalRow(char* out, size_t cap, size_t width, const Vehicle::Meta& m, int32_t v,
                   bool fresh, bool verified)
    {
        if (!cap) return;
        char val[24];
        signalValue(val, sizeof(val), m, v, fresh, verified);
        lay(out, cap, width, m.name, val);
    }

    void tripRows(char* const rows[3], size_t cap, size_t width, const TripVal& inst,
                  const TripVal& avg, const TripVal& range, const TripVal& km)
    {
        if (!cap) return;
        // x10 with one decimal, or "--".
        auto x10 = [](char* o, size_t n, const TripVal& t) {
            if (!t.fresh) { snprintf(o, n, "--"); return; }
            const int32_t a = t.v < 0 ? -t.v : t.v;
            snprintf(o, n, "%s%ld.%ld", t.v < 0 ? "-" : "", (long)(a / 10), (long)(a % 10));
        };
        char a[12], b[12], val[28];
        x10(a, sizeof(a), inst);
        x10(b, sizeof(b), avg);
        const bool guess = (inst.fresh && !inst.verified) || (avg.fresh && !avg.verified);
        snprintf(val, sizeof(val), "%s/%s%s", a, b, guess ? "?" : "");
        lay(rows[0], cap, width, "L/100km", val);

        signalValue(val, sizeof(val), Vehicle::meta(Vehicle::SIG_RANGE_CALC), range.v, range.fresh,
                    range.verified || !range.fresh);
        lay(rows[1], cap, width, "Range", val);
        const bool kmShown = km.fresh || km.seen;
        signalValue(val, sizeof(val), Vehicle::meta(Vehicle::SIG_TRIP_KM), km.v, kmShown,
                    !kmShown || (km.fresh && km.verified));
        lay(rows[2], cap, width, "Trip", val);
    }
}
