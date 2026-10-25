#pragma once
#include "../Features.h"

// One TLS job at a time: the GitHub update (GhOta) and the log push (LogPush).
//
// WHY: both move mbedTLS's allocator into PSRAM for their job, and that is a
// global. Two jobs at once means the first to finish hands the other back to
// internal RAM in the middle of its handshake (~40 kB there, most of what is
// free). So a job claims this first, and the loser waits for the next try.
#if FEATURE_GH_OTA || FEATURE_LOG_PUSH
namespace TlsJob
{
    // false: another job holds it (holder() says which).
    bool        claim(const char* who);
    void        release();
    const char* holder();     // nullptr when free

    // For the length of a job: mbedTLS allocates in PSRAM, internal RAM when
    // PSRAM is full. heap_caps_free frees either, so the swap back is safe
    // with blocks still out. on = false: no PSRAM, nothing was swapped.
    struct InPsram {
        bool on = false;
        InPsram();
        ~InPsram();
    };
}
#endif
