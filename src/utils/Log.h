#pragma once
#include <stdint.h>

enum class LogLevel : uint8_t { ERR = 0, WRN = 1, INF = 2, DBG = 3, TRC = 4 };

namespace Log {
    // Row i = LogLevel i; /api/loglevel and /api/catalog (logLevels) read it.
    inline constexpr uint8_t LEVELS = 5;
    inline constexpr const char* LEVEL_NAMES[LEVELS] = {"ERR", "WRN", "INF", "DBG", "TRC"};
    // The web log's tag groups (/api/catalog logTags, /api/logs `tag`): the
    // [cat] a line starts with, looked up in `cats` (lower case, space-separated;
    // LogLine::parse in LogRing.cpp). A new [cat] goes in a row here; one in no
    // row, or a line with no [cat], is the last row.
    struct TagRow { const char* tag; const char* cats; };
    inline constexpr TagRow TAG_ROWS[] = {
        {"KEY", "kev key panelkey res knobs enc hid usb"},
        {"PNL", "disp panel affa ui clock"},
        {"BLE", "ble bt ams cts ancs"},
        {"HU",  "hu hur media"},
        {"CAN", "can can2 twai car link boardv"},
        {"CAP", "cap"},
        {"MAC", "macro"},
        {"NET", "net wifi web cloud"},
        {"SYS", ""},   // boot, hb, mem, task, stall, stk, log, cfg, idf, ...
    };
    inline constexpr uint8_t TAG_COUNT = sizeof(TAG_ROWS) / sizeof(TAG_ROWS[0]);
}

// Native (host) test build: no-op stubs so the portable logic layer links
// without the firmware logger.
#ifdef NATIVE
namespace Log {
    inline void        setLevel(LogLevel) {}
    inline LogLevel    level() { return LogLevel::INF; }
    inline bool        enabled(LogLevel) { return false; }
    inline const char *levelName(LogLevel) { return ""; }
    inline void        emit(LogLevel, const char *, const char *, ...) {}
    inline void        printf(const char *, ...) {}
}
#define LOGE(cat, ...) ((void)0)
#define LOGW(cat, ...) ((void)0)
#define LOGI(cat, ...) ((void)0)
#define LOGD(cat, ...) ((void)0)
#define LOGT(cat, ...) ((void)0)
#else // ---- firmware build ------------------------------------------------------
#include <Arduino.h>

// ============================================================================
// Leveled, categorized logger.
//
// Format: "<L> [<cat>] <message>", L = E/W/I/D/T. Log::emit -> LogBus::line ->
// the LogBus ring, which fans out to
// Serial, the flash log and the cloud and keeps sent lines for the web log
// (GET /api/logs?since=, LogRing.h).
//
//   ERR  failure; WRN  unexpected but handled; INF  state transitions;
//   DBG  per-operation narration; TRC  per-frame firehose.
//
// Runtime threshold (default INF) via HTTP /api/loglevel?n=<0..4> (serial input
// is dead on the C3 USB-CDC). Not persisted: TRC costs the CAN path real time.
// Gated-out lines return before formatting; guard costly payload builds with
// Log::enabled(). Fixed stack buffer, no String: safe in the CAN callback.
// ============================================================================

namespace Log {
    void     setLevel(LogLevel lvl);
    LogLevel level();
    bool     enabled(LogLevel lvl);   // true when lvl <= current threshold
    const char *levelName(LogLevel lvl);

    void emit(LogLevel lvl, const char *cat, const char *fmt, ...)
        __attribute__((format(printf, 3, 4)));

    // printf() = INF with no category.
    void   printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

// Category: short stable literal ("BT", "CAN", "AFFA", ...); used as a filter key.
#define LOGE(cat, ...) Log::emit(LogLevel::ERR, cat, __VA_ARGS__)
#define LOGW(cat, ...) Log::emit(LogLevel::WRN, cat, __VA_ARGS__)
#define LOGI(cat, ...) Log::emit(LogLevel::INF, cat, __VA_ARGS__)
#define LOGD(cat, ...) Log::emit(LogLevel::DBG, cat, __VA_ARGS__)
#define LOGT(cat, ...) Log::emit(LogLevel::TRC, cat, __VA_ARGS__)
#endif // NATIVE
