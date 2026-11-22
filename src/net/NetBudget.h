#pragma once
#include <stdint.h>

// The internal-heap budget a network job asks before it starts
// (docs/design/stabilisation-1-12.md S3, rule R3). Background jobs (the log
// push in every kind: log part, status, coredump, past flash log, CAN
// recording) are held while it says no and go on the next pass; a user's
// GitHub check or install never waits in silence: it runs, or the page gets
// e.heap_low at once.
//
// WHAT THE NUMBERS ARE (the build's sdkconfig, qio_opi, IDF 4.4; nothing
// here is a guess at a "safe" number, each line is a count):
//   mbedTLS: 0 internal. NetJob moves its allocator into PSRAM for the job
//     (NetJob.cpp, InPsram); both record buffers (MAX_CONTENT_LEN 16384
//     each) and the handshake's ~40 kB go there.
//   HTTP client buffers: 0 internal. buffer_size(_tx) 4608 is above
//     SPIRAM_MALLOC_ALWAYSINTERNAL (4096), so malloc puts them in PSRAM.
//   What stays internal is the lwIP/WiFi side of the one connection:
//   - lwIP uses plain malloc (lwipopts.h MEM_LIBC_MALLOC, MEMP_MEM_MALLOC;
//     no PSRAM hook in this build), and every block is under 4096: the
//     unacked TX segments, at most TCP_SND_BUF 5760 = 4 MSS (1436) with
//     headers, ~6.5 kB (a PUT); the pcb, netconn, socket, DNS: ~0.5 kB.
//   - WiFi RX: the dynamic RX buffers (DMA, internal) of the window,
//     TCP_WND 5760 = 4 MSS, ~1.6 kB each with the header, ~6.5 kB (a GET).
//     One direction carries data at a time (a PUT gets ACKs back, a GET
//     sends them), so the peak is one of the two, not their sum. WiFi TX
//     copies into its 8 static buffers, allocated at WiFi start.
//   - Small allocations: the client handle, its header and URL strings, the
//     transports, esp_tls, getaddrinfo; each under 4096: ~2 kB.
//   - The install's flash-write chunk (GhOta CHUNK, internal on purpose):
//     1 kB, plus esp_ota's handle.
//   Sum ~10 kB: NEED_WIFI 12 kB.
//   Through the app (AppLink): no socket and no TLS on the board; the link's
//   buffers were taken once, in PSRAM (AppLink.cpp, LinkSession.cpp); the
//   chunk and small strings only: NEED_APP 2 kB.
// RESERVE is what the rest of the board must still find while the job runs:
// one web request's small blocks (PsychicHttp, ArduinoJson pools under
// 4096 B are internal) and a BLE event burst, ~8 kB. NEED_WIFI + RESERVE =
// 20 kB, the line under which the [mem] low guard (S6) warns: a job never
// starts on a heap that would put the board there.
//
// Largest block: only what the path's biggest single internal allocation
// needs (a WiFi RX buffer, ~1.6 kB; the 1 kB chunk), never more. A largest
// block of ~10 kB is the car's normal, and a gate above that would hold
// every push for good.
//
// The verdict is pure (verdict() below, host-tested in test/test_netbudget);
// admit() reads the heap and asks it.
namespace NetBudget
{
    enum class Path : uint8_t { Wifi, App };

    constexpr uint32_t NEED_WIFI = 12 * 1024;
    constexpr uint32_t NEED_APP  = 2 * 1024;
    constexpr uint32_t RESERVE   = 8 * 1024;
    constexpr uint32_t BIG_WIFI  = 2 * 1024;
    constexpr uint32_t BIG_APP   = 1280;

    // Free internal heap a job on `p` needs before it starts.
    constexpr uint32_t needFree(Path p) { return (p == Path::Wifi ? NEED_WIFI : NEED_APP) + RESERVE; }
    // The largest free internal block it needs.
    constexpr uint32_t needBig(Path p) { return p == Path::Wifi ? BIG_WIFI : BIG_APP; }

    // free, big: internal heap now (bytes). True: the job may start.
    constexpr bool verdict(Path p, uint32_t freeNow, uint32_t bigNow)
    {
        return freeNow >= needFree(p) && bigNow >= needBig(p);
    }

    struct Sample {
        Path     path = Path::Wifi;
        uint32_t free = 0, big = 0, need = 0;
    };
    // On the device: the path the next request takes (AppLink::netFirst()),
    // the internal heap now, the verdict. `out`, if given, says what was
    // read (the held line prints it). The largest block is read only when the
    // free size passes (it walks the heap).
    bool admit(Sample* out = nullptr);
}
