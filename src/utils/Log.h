#pragma once
#include <stdint.h>

enum class LogLevel : uint8_t { ERR = 0, WRN = 1, INF = 2, DBG = 3, TRC = 4 };

namespace Log {
    // Row i = LogLevel i; the settings registry (logLevel) and /api/catalog
    // (logLevels) read it.
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
        {"CAN", "can can2 twai car"},
        {"CAP", "cap"},
        {"MAC", "macro"},
        {"NET", "net wifi web"},
        {"SYS", ""},   // boot, hb, mem, task, stall, stk, log, cfg, idf, ...
    };
    inline constexpr uint8_t TAG_COUNT = sizeof(TAG_ROWS) / sizeof(TAG_ROWS[0]);

    // Where a line can go (docs/design/logging.md): each takes lines up to its
    // own level, set by logWeb / logFlash / logPush / logSerial.
    enum Sink : uint8_t { SINK_WEB = 0, SINK_FLASH, SINK_PUSH, SINK_SERIAL, SINK_COUNT };
}

// Native (host) test build: no-op stubs so the portable logic layer links
// without the firmware logger.
#ifdef NATIVE
namespace Log {
    // Kept, so the settings registry's log level reads back what it set.
    inline LogLevel&   levelRef() { static LogLevel l = LogLevel::INF; return l; }
    inline void        setLevel(LogLevel lvl) { levelRef() = lvl; }
    inline LogLevel    level() { return levelRef(); }
    inline bool        setTagSpec(const char*) { return true; }
    inline void        setSinkLevel(Sink, LogLevel) {}
    inline bool        on(LogLevel, const char*, int8_t&) { return false; }
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
// Serial and the flash log and keeps sent lines for the web log
// (GET /api/logs?since=, LogRing.h).
//
//   ERR  failure; WRN  unexpected but handled; INF  state transitions;
//   DBG  per-operation narration; TRC  per-frame firehose.
//
// Runtime threshold (default INF) via the settings registry's logLevel
// (/api/settings, the panel menu; saved), per tag via logTags, per sink via
// logWeb / logFlash / logPush / logSerial (docs/design/logging.md).
// Gated-out lines return before formatting; guard costly payload builds with
// Log::enabled(). Fixed stack buffer, no String: safe in the CAN callback.
// ============================================================================

namespace Log {
    void     setLevel(LogLevel lvl);
    LogLevel level();
    bool     enabled(LogLevel lvl);   // true when lvl <= the global threshold
    const char *levelName(LogLevel lvl);

    // Per-tag thresholds, the setting logTags ("hu=TRC knobs=DBG"); false on a
    // bad spec (nothing changes). LogTags.h.
    bool setTagSpec(const char* spec);
    const char* tagSpec();
    // The tags seen this boot, with their override (LogTags::NONE: global).
    uint8_t     tagCount();
    const char* tagName(uint8_t i);
    uint8_t     tagOverride(uint8_t i);

    // The lowest level a sink takes; a line goes to it when it passed its
    // tag's threshold AND its level is at most the sink's. The flash log never
    // takes TRC (every frame would erase sectors): asked for, it gets DBG.
    void     setSinkLevel(Sink s, LogLevel lvl);
    LogLevel sinkLevel(Sink s);
    // For a sink: does this line ("<L> [tag] ...", or a legacy line = INF) go?
    bool     sinkTakes(Sink s, const char* line, int len);

    // Would a line of this level and tag be written? `slot` caches the tag's
    // index (start it at -1): the macros keep one per call site.
    bool on(LogLevel lvl, const char* tag, int8_t& slot);
    // Formats and writes, no threshold check (the macros checked).
    void write(LogLevel lvl, const char* tag, const char* fmt, ...)
        __attribute__((format(printf, 3, 4)));

    // Checks the threshold itself (looks the tag up every call): for code that
    // cannot use the macros.
    void emit(LogLevel lvl, const char *cat, const char *fmt, ...)
        __attribute__((format(printf, 3, 4)));

    // printf() = INF with no category.
    void   printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

// Category: short stable literal ("BT", "CAN", "AFFA", ...); used as a filter key.
// The tag is a string literal; its index is looked up once per call site, and
// the arguments are evaluated only when the line will be written.
#define LOG_AT(lvl, cat, ...)                                                        \
    do {                                                                             \
        static int8_t s_logTag_ = -1;                                                \
        if (Log::on(lvl, cat, s_logTag_)) Log::write(lvl, cat, __VA_ARGS__);         \
    } while (0)
#define LOGE(cat, ...) LOG_AT(LogLevel::ERR, cat, __VA_ARGS__)
#define LOGW(cat, ...) LOG_AT(LogLevel::WRN, cat, __VA_ARGS__)
#define LOGI(cat, ...) LOG_AT(LogLevel::INF, cat, __VA_ARGS__)
#define LOGD(cat, ...) LOG_AT(LogLevel::DBG, cat, __VA_ARGS__)
#define LOGT(cat, ...) LOG_AT(LogLevel::TRC, cat, __VA_ARGS__)
#endif // NATIVE
