#include "Log.h"

#include <stdarg.h>
#include <string.h>
#include "LogBus.h"
#include "LogRing.h"
#include "LogTags.h"
#include "freertos/FreeRTOS.h"

// Leveled logger. Formats "<L> [cat] msg" into a fixed stack buffer and hands it to
// the LogBus. No String, no heap.
namespace Log
{
    namespace
    {
        volatile LogLevel g_level = LogLevel::INF;
        const char        LVL_CH[5]   = {'E', 'W', 'I', 'D', 'T'};
        LogTags           g_tags;
        portMUX_TYPE      g_tagMux = portMUX_INITIALIZER_UNLOCKED;
        // web TRC, flash DBG, push TRC, serial INF (the settings' defaults)
        volatile uint8_t  g_sink[SINK_COUNT] = {4, 3, 4, 2};
        // Status lines: when each tag last wrote one, and how often at INF.
        uint32_t          g_statusAt[LogTags::MAX] = {};
        volatile uint32_t g_statusMs = 60000;

        // Strip trailing CR/LF so each record is exactly one line (many legacy call
        // sites pass a trailing newline).
        void rstripEol(char *s)
        {
            size_t n = strlen(s);
            while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
                s[--n] = 0;
        }
    } // namespace

    void setLevel(LogLevel lvl) { g_level = lvl; }
    LogLevel level() { return g_level; }
    bool enabled(LogLevel lvl) { return (uint8_t)lvl <= (uint8_t)g_level; }

    const char *levelName(LogLevel lvl)
    {
        uint8_t i = (uint8_t)lvl;
        return i < LEVELS ? LEVEL_NAMES[i] : "?";
    }

    bool on(LogLevel lvl, const char* tag, int8_t& slot)
    {
        if (slot < 0) {
            portENTER_CRITICAL(&g_tagMux);
            slot = g_tags.index(tag);
            portEXIT_CRITICAL(&g_tagMux);
        }
        return (uint8_t)lvl <= g_tags.threshold(slot, (uint8_t)g_level);
    }

    bool statusOn(const char* tag, int8_t& slot, uint32_t now)
    {
        if (!on(LogLevel::INF, tag, slot)) return false;
        if (on(LogLevel::DBG, tag, slot)) return true;          // DBG: every window
        if (slot < 0) return true;                               // table full: as before
        uint32_t& at = g_statusAt[slot];
        if (at && (uint32_t)(now - at) < g_statusMs) return false;
        at = now ? now : 1;
        return true;
    }
    void     setStatusInterval(uint32_t ms) { g_statusMs = ms < 1000 ? 1000 : ms; }
    uint32_t statusInterval()               { return g_statusMs; }

    bool setTagSpec(const char* spec)
    {
        portENTER_CRITICAL(&g_tagMux);
        const bool ok = g_tags.setSpec(spec);
        portEXIT_CRITICAL(&g_tagMux);
        return ok;
    }
    const char* tagSpec()              { return g_tags.spec(); }
    uint8_t     tagCount()             { return g_tags.count(); }
    const char* tagName(uint8_t i)     { return g_tags.name(i); }
    uint8_t     tagOverride(uint8_t i) { return g_tags.override_(i); }

    void setSinkLevel(Sink s, LogLevel lvl)
    {
        if (s >= SINK_COUNT) return;
        if (s == SINK_FLASH && lvl > LogLevel::DBG) lvl = LogLevel::DBG;
        g_sink[s] = (uint8_t)lvl;
    }
    LogLevel sinkLevel(Sink s) { return s < SINK_COUNT ? (LogLevel)g_sink[s] : LogLevel::INF; }

    bool sinkTakes(Sink s, const char* line, int len)
    {
        if (s >= SINK_COUNT || len <= 0) return false;
        return LogLine::parse(line, (uint32_t)len).level <= g_sink[s];
    }

    void write(LogLevel lvl, const char *cat, const char *fmt, ...)
    {
        char line[LogBus::MAX_LINE];
        int p = snprintf(line, sizeof(line), "%c [%s] ", LVL_CH[(uint8_t)lvl], cat ? cat : "");
        if (p < 0)
            return;
        if (p > (int)sizeof(line) - 1)
            p = sizeof(line) - 1;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line + p, sizeof(line) - p, fmt, ap);
        va_end(ap);
        rstripEol(line);
        LogBus::line(line);
    }

    void emit(LogLevel lvl, const char *cat, const char *fmt, ...)
    {
        int8_t slot = -1;
        if (!on(lvl, cat, slot))
            return;
        char line[LogBus::MAX_LINE];
        int p = snprintf(line, sizeof(line), "%c [%s] ", LVL_CH[(uint8_t)lvl], cat ? cat : "");
        if (p < 0)
            return;
        if (p > (int)sizeof(line) - 1)
            p = sizeof(line) - 1;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line + p, sizeof(line) - p, fmt, ap);
        va_end(ap);
        rstripEol(line);
        LogBus::line(line);
    }

    // --- back-compat shims --------------------------------------------------------
    void printf(const char *fmt, ...)
    {
        if ((uint8_t)LogLevel::INF > (uint8_t)g_level)
            return;
        char line[LogBus::MAX_LINE];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);
        rstripEol(line);
        LogBus::line(line);
    }

}
