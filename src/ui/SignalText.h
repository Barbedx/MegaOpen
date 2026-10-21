#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../vehicle/Signals.h"

// One car signal as the panel prints it: the ASCII name on the left, the
// value with its unit flush right, "?" after a value whose decode no car has
// confirmed yet, "--" for one too old to trust. The panel menu's Car data rows
// and the car screen both use it, so they never disagree.
//
// Pure (no Arduino): native-tested.
namespace MenuUi
{
    // A decoded value older than this is shown as "--".
    inline constexpr uint32_t STALE_MS = 5000;

    // The value alone: "92C", "14.12V", "on", "--" when !fresh, then "?" when
    // !verified. `out` gets at most cap-1 characters.
    void signalValue(char* out, size_t cap, const Vehicle::Meta& m, int32_t v, bool fresh, bool verified);

    // "name ... value" laid out on exactly `width` columns (cut to cap-1).
    void signalRow(char* out, size_t cap, size_t width, const Vehicle::Meta& m, int32_t v,
                   bool fresh, bool verified);

    // The car screen's «Поїздка» preset (Cfg::CarPreset::Trip), three rows on
    // `width` columns from the derived trip signals:
    //   "L/100km    7.2/6.8"   cons_inst / cons_avg ("--" for either one missing)
    //   "Range        420km"   range_calc
    //   "Trip       123.4km"   trip_km
    // "--" with no data; "?" after a value not yet verified, as everywhere else.
    struct TripVal { int32_t v; bool fresh, verified; };
    void tripRows(char* const rows[3], size_t cap, size_t width, const TripVal& inst,
                  const TripVal& avg, const TripVal& range, const TripVal& km);
}
