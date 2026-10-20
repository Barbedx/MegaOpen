#pragma once
#include <stdint.h>

// The liveness rule behind the OTA verdict, apart from the registry (which
// needs millis() and a portMUX) so the host tests can reach it. An image whose
// critical task stops coming round is rolled back on the strength of this, so
// it is the one rule in the registry that must not be wrong.
namespace App
{
    namespace Tasks
    {
        // Signed difference: a task may beat between the caller reading nowMs
        // and this read, which is "just now", not 49 days ago. Unsigned
        // subtraction first, so a millis() wrap in between is harmless too.
        inline bool beatWithin(uint32_t nowMs, uint32_t lastAliveMs, uint32_t maxAgeMs)
        {
            return (int32_t)(nowMs - lastAliveMs) <= (int32_t)maxAgeMs;
        }

        // The first critical entry that has not beaten within maxAgeMs, or -1.
        // Non-critical entries never count. Entry needs `critical` and
        // `lastAliveMs`; templated so the tests can pass a plain struct.
        template <class Entry>
        int firstLateCritical(const Entry* e, int n, uint32_t nowMs, uint32_t maxAgeMs)
        {
            for (int i = 0; i < n; ++i) {
                if (!e[i].critical) continue;
                if (!beatWithin(nowMs, e[i].lastAliveMs, maxAgeMs)) return i;
            }
            return -1;
        }
    }
}
