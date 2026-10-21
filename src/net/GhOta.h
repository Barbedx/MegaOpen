#pragma once
#include <stdint.h>
#include "../Features.h"

// Firmware updates from GitHub Releases (docs/BACKLOG.md), only ever from the
// Плата page: check() reads /releases/latest and compares its tag with
// MEGAOPEN_VERSION; install() downloads this env's megaopen-<env>-<ver>.bin
// into the free OTA slot while hashing it, sets the boot partition only when
// the SHA-256 matches the release's SHA256SUMS, and reboots into the same
// trial/rollback as the local upload (src/app/OtaTrial.h).
//
// Both run on a task of their own at the lowest priority on core 0, created
// for the job and deleted after it, so the keys and BLE never wait on TLS. On
// a board with PSRAM, mbedTLS allocates there for the job (it is pinned to
// internal RAM by the prebuilt sdkconfig), and its ~40 kB stays off the heap.
//
// The routes (WebUiGhOta.cpp) gate the rest: STA only, never with the car
// moving.

#if FEATURE_GH_OTA
namespace GhOta
{
    enum class Phase : uint8_t {
        Idle,          // never checked this boot
        Checking,
        Current,       // the latest release is not newer than this image
        Available,     // a newer release has an image for this env
        Downloading,
        Rebooting,     // hash matched, the boot partition is set
        Failed,        // err says why; check() again
    };
    const char* phaseId(Phase p);

    struct View {
        Phase    phase = Phase::Idle;
        char     latest[24] = "";      // the release's tag
        char     asset[64]  = "";
        uint32_t size = 0, got = 0;    // bytes: the asset's, and downloaded so far
        char     err[96] = "";
        // Internal heap, bytes: free when the job started, and the lowest seen
        // during it (the TLS cost the BACKLOG asks to measure).
        uint32_t heapStart = 0, heapMin = 0;
        // ...and at the steps between: the task running (its stack is
        // internal), the HTTP client made, the last TLS connection open.
        uint32_t heapTask = 0, heapClient = 0, heapTls = 0;
        uint32_t stackFree = 0;        // the last job's unused stack, bytes
        bool     tlsInPsram = false;
    };
    void view(View& out);

    // nullptr: the job started. Otherwise why not (a job is running, or for
    // install(), no newer release from a check this boot).
    const char* check();
    const char* install();
}
#endif
