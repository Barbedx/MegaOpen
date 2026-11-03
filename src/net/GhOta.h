#pragma once
#include <stdint.h>
#include "../Features.h"

// Firmware updates from GitHub Releases (docs/BACKLOG.md), only ever from the
// Плата page: check() reads /releases/latest and compares its tag with
// MEGAOPEN_VERSION; install() downloads this env's megaopen-<env>-<ver>.bin
// into the free OTA slot while hashing it, sets the boot partition only when
// the SHA-256 matches the release's SHA256SUMS, and then reboots into the
// trial/rollback (src/app/OtaTrial.h) at once, as after a file upload. No
// gate on the car moving (the owner's call, 2026-11-02: he updates on the
// move, and the trial runs anywhere).
//
// One OTA owner: while a job runs, the local upload (/ota/start,
// /ota/upload) is refused (WebUi::gateLocalOta), and an install refuses a
// local upload in progress, but clears one left open by a /ota/start that
// never uploaded (LOCAL_STALE_MS).
//
// Both run on the network worker (NetJob.h), at the lowest priority on core
// 0, so the keys and BLE never wait on TLS. On a board with PSRAM, mbedTLS
// allocates there for the job (it is pinned to internal RAM by the prebuilt
// sdkconfig), and its ~40 kB stays off the heap.
//
// The routes (WebUiGhOta.cpp) gate the rest: STA only.

#if FEATURE_GH_OTA
namespace GhOta
{
    constexpr uint32_t LOCAL_STALE_MS  = 60000;   // a local session with no chunk this long
    enum class Phase : uint8_t {
        Idle,          // never checked this boot
        Checking,
        Current,       // the latest release is not newer than this image
        Available,     // a newer release has an image for this env
        Downloading,
        Written,       // hash matched, the boot partition is set; reboot pending
        Rebooting,
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
        // ...and at the steps between: the job just queued, the job running,
        // the HTTP client made, the last TLS connection open.
        uint32_t heapCreated = 0, heapTask = 0, heapClient = 0, heapTls = 0;
        uint32_t stackFree = 0;        // the last job's unused stack, bytes
        // The download in tenths: internal heap at its lowest in each (our
        // own samples, and the system-wide low-water mark when it moved
        // inside that tenth, which catches the dips while this task waits),
        // and how long each tenth took. 0 = not reached.
        uint32_t stepMin[10] = {}, stepMs[10] = {};
        bool     tlsInPsram = false;
        bool     pre = false;          // the test channel (pre())
        // The job waits its turn: the log push's part (seconds) is on the
        // worker, and the job goes next (NetJob).
        bool     queued = false;
    };
    void view(View& out);

    // The channel a check reads, NVS "fw"/"pre": false (default) = the latest
    // final only; true = the newest of the ten newest releases, an rc
    // included (GhRelease::pickNewest). httpd; refused while a job runs.
    bool        pre();
    const char* setPre(bool on);

    // nullptr: the job started. Otherwise why not (a job is running, or for
    // install(), no newer release from a check this boot).
    const char* check();
    const char* install();
    // A job owns the OTA slot: the local upload must wait.
    bool busy();
    // ElegantOTA's hooks: a local session began, sent a chunk, or ended.
    void localStart();
    void localChunk();
    void localEnd();
    // The loop: the idle watchdog back on after an upload that never ended.
    void localTick();
}
#endif
