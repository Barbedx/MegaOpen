#pragma once
#include <AffaTwai.h>
#include <stdio.h>

// The library's log lines, formatted "<L> [tag] msg" and forwarded. Only the
// link header: env:direct, which has no panel, uses it too (affa::setLogSink).
namespace MegaOpen
{
    // A sink is a leaf: it can be called mid-transition and must never call back
    // into the library, so it only formats and forwards.
    class AffaLogSink final : public affa::ILogSink {
    public:
        using EmitFn = void (*)(const char* line);
        void begin(EmitFn e) { _emit = e; }
        void write(uint8_t level, const char* tag, const char* msg) override
        {
            if (!_emit) return;
            static const char kL[] = "-EWIDT";
            char b[200];
            snprintf(b, sizeof(b), "%c [%s] %s", kL[level <= 5 ? level : 0],
                     tag ? tag : "?", msg ? msg : "");
            _emit(b);
        }

    private:
        EmitFn _emit = nullptr;
    };
}
