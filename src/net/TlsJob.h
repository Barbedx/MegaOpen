#pragma once
#include "../Features.h"

// One TLS job at a time: the GitHub update (GhOta) and the log push (LogPush).
//
// WHY: both move mbedTLS's allocator into PSRAM for their job, and that is a
// global. Two jobs at once means the first to finish hands the other back to
// internal RAM in the middle of its handshake (~40 kB there, most of what is
// free). So a job claims this first, and the loser waits for the next try.
// The update the owner pressed must not lose to the push's next part: it
// reserves the next turn, and every other claim fails until it has claimed.
#if FEATURE_GH_OTA || FEATURE_LOG_PUSH
namespace TlsJob
{
    // false: another job holds it (holder() says which), or `who` is not the
    // one that reserved the next turn.
    bool        claim(const char* who);
    void        release();
    // The next turn is `who`'s (one at a time; a second reserve() replaces
    // it); its claim() clears it, unreserve() gives it up.
    void        reserve(const char* who);
    void        unreserve(const char* who);
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
