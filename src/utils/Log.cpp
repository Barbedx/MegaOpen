#include "Log.h"

#include <stdarg.h>
#include <string.h>
#include "LogBus.h"

// Leveled logger. Formats "<L> [cat] msg" into a fixed stack buffer and hands it to
// the LogBus. No String, no heap.
namespace Log
{
    namespace
    {
        volatile LogLevel g_level = LogLevel::INF;
        const char        LVL_CH[5]   = {'E', 'W', 'I', 'D', 'T'};

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

    void emit(LogLevel lvl, const char *cat, const char *fmt, ...)
    {
        if ((uint8_t)lvl > (uint8_t)g_level)
            return;
        char line[200];
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
        char line[200];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);
        rstripEol(line);
        LogBus::line(line);
    }

}
