#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../Features.h"

// The log, pushed to a private GitHub repository whenever the board has
// internet (docs/BACKLOG.md "remote logs"): the owner reads a drive from home
// while the car is out on the head unit's hotspot.
//
// Every line also goes into a spool in PSRAM (SPOOL bytes, ~an hour and a
// half of routine logging). Once a minute, on STA with a token set, a job
// takes what came since the last push and PUTs it as one file:
//   <repo>/<device>/boot-<sys/boots>/<part>.log
// through the contents API (one commit per file). Without internet the spool
// keeps filling; the next push after the link comes back sends what it holds.
// A spool that wraps before then loses its oldest lines, and the next file
// says how many bytes.
//
// The token (fine-grained, Contents read/write on that one repository) and
// the switch live in NVS namespace "logpush"; the token is never sent back.
//
// The job runs on a task of its own at the lowest priority on core 0, created
// for the push and deleted after it, like GhOta's; TLS in PSRAM, one TLS job
// at a time (TlsJob.h). It never runs below MIN_HEAP of internal heap.
#if FEATURE_LOG_PUSH
namespace LogPush
{
    constexpr size_t   SPOOL       = 1024 * 1024;
    constexpr size_t   SPOOL_SMALL = 128 * 1024;   // when PSRAM cannot spare SPOOL
    constexpr size_t   CHUNK_MAX   = 64 * 1024;    // one file, before base64
    constexpr uint32_t PERIOD_MS   = 60000;
    constexpr uint32_t BACKOFF_MAX_MS = 10 * 60000;
    constexpr uint32_t MIN_HEAP    = 40 * 1024;
    constexpr size_t   TOKEN_MAX   = 255;
    constexpr size_t   REPO_MAX    = 100;          // "owner/name"
    constexpr const char* REPO_DEFAULT = "Barbedx/megaopen-logs";
    // The CAN recording: closed /cap files, in parts of CAN_PART bytes, as
    //   <repo>/<device>/cap/<session>/<file>-<part>.bin
    // (tools/cap_fetch.py joins them; tools/can_dump.py reads the file). Off
    // by default; on, it also starts the recorder. At most canMB a day.
    constexpr size_t   CAN_PART = 512 * 1024;
    constexpr uint32_t CAN_MB_DEFAULT = 200;
    constexpr uint32_t CAN_MB_MAX = 5000;

    // First thing after LogBus::grow(), before any sink: the spool, the NVS
    // settings. false: no memory for a spool; everything else is a no-op.
    bool begin();
    // A LogBus::Sink (main.cpp filters it like the flash log).
    void sink(const char* line, int len, uint32_t ms);
    // net_svc, every pass: starts a push when one is due.
    void tick(uint32_t now, bool online);

    enum class State : uint8_t {
        Off,        // switched off, or no token
        Waiting,    // on; the next push when due and online
        Pushing,
        Stopped,    // GitHub refused the token or the repository: err; fix and save
    };
    const char* stateId(State s);

    struct View {
        State    state = State::Off;
        bool     on = false, hasToken = false;
        char     repo[REPO_MAX + 1] = "";
        char     device[16] = "";
        char     err[48] = "";          // a code for the page (e.<code>), "" none
        int      lastStatus = 0;        // HTTP status of the last try, 0 none
        uint32_t files = 0;             // pushed this boot
        uint32_t sentBytes = 0;
        uint32_t lostBytes = 0;         // spool wrapped before they were pushed
        uint32_t queuedBytes = 0;       // in the spool, not pushed yet
        uint32_t spoolBytes = 0;        // the spool's size
        uint32_t lastOkAgoMs = 0;       // 0: none this boot
        uint32_t nextInMs = 0;
        uint32_t heapMin = 0;           // internal heap low during the last job
        // The car's CAN recording (src/capture/CanRecorder.h), sent too.
        bool     canOn = false;
        uint32_t canMB = CAN_MB_DEFAULT; // a day at most
        uint32_t canToday = 0;          // bytes sent today (UTC; while the date is unknown, the last known day)
        uint32_t canFile = 0;           // the recording file being sent, 0 none
        bool     canBudget = false;     // today's limit reached
    };
    void view(View& out);

    // httpd. Each argument may be null (unchanged); an empty token keeps the
    // stored one (the page never shows it). Switching off is how to stop.
    // nullptr: saved; else an error code.
    const char* set(const bool* on, const char* token, const char* repo);
    // The CAN part: on/off, the day's limit in MB (1..CAN_MB_MAX). Same rules.
    const char* setCan(const bool* on, const uint32_t* mb);
    // The page's "push now": the next tick starts a push when online.
    void pushSoon();
}
#endif
