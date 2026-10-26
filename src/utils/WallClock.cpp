#include "WallClock.h"
#include <sys/time.h>
#include <time.h>
#include "LogBus.h"

namespace WallClock
{
    namespace
    {
        constexpr int64_t MIN_MS = 1767225600000LL;   // 2026-01-01: anything earlier is no clock
        constexpr int64_t MAX_MS = 4102444800000LL;   // 2100
        constexpr int64_t SLACK_MS = 5000;
    }

    bool known()
    {
        return (int64_t)time(nullptr) * 1000 >= MIN_MS;
    }

    bool fromApp(int64_t epochMs, const char* via)
    {
        if (epochMs < MIN_MS || epochMs > MAX_MS) return false;
        timeval now;
        gettimeofday(&now, nullptr);
        const int64_t nowMs = (int64_t)now.tv_sec * 1000 + now.tv_usec / 1000;
        const int64_t off = epochMs - nowMs;
        const bool was = known();
        if (was && off < SLACK_MS && off > -SLACK_MS) return true;
        timeval tv;
        tv.tv_sec  = (time_t)(epochMs / 1000);
        tv.tv_usec = (suseconds_t)((epochMs % 1000) * 1000);
        if (settimeofday(&tv, nullptr) != 0) return false;
        struct tm t;
        gmtime_r(&tv.tv_sec, &t);
        char s[24];
        strftime(s, sizeof(s), "%Y-%m-%d %H:%M:%S", &t);
        if (was) LogBus::printf("[time] %s UTC from the app (%s), was %lld s off", s, via, (long long)(off / 1000));
        else     LogBus::printf("[time] %s UTC from the app (%s)", s, via);
        return true;
    }
}
