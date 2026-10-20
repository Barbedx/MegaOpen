#include "AffaPort.h"
#include <string.h>

namespace MegaOpen
{
    void AffaLogSink::write(uint8_t level, const char* tag, const char* msg)
    {
        if (!_emit) return;
        static const char kL[] = "-EWIDT";
        char b[200];
        snprintf(b, sizeof(b), "%c [%s] %s", kL[level <= 5 ? level : 0],
                 tag ? tag : "?", msg ? msg : "");
        _emit(b);
    }

    affa::AffaDisplayBase* createDisplay(const char* type,
                                         affa::ICanLink& link, affa::IClock& clock,
                                         affa::CarminatDisplay** carminat)
    {
        if (carminat) *carminat = nullptr;
        if (!type) type = "carminat";
        if (!strcmp(type, "carminat")) {
            auto* c = new affa::CarminatDisplay(link, clock);
            if (carminat) *carminat = c;
            return c;
        }
        if (!strcmp(type, "updatelist_menu")) return new affa::UpdateListMenuDisplay(link, clock);
        return new affa::UpdateListDisplay(link, clock);
    }

    const char* resultName(affa::Result r)
    {
        switch (r) {
            case affa::Result::Ok:           return "ok";
            case affa::Result::NoSync:       return "no-sync";
            case affa::Result::UnknownFunc:  return "unknown-func";
            case affa::Result::SendFailed:   return "send-failed";
            case affa::Result::Timeout:      return "timeout";
            case affa::Result::TooLong:      return "too-long";
            case affa::Result::QueueFull:    return "queue-full";
            case affa::Result::NotSupported: return "not-supported";
            case affa::Result::BadArgument:  return "bad-argument";
            case affa::Result::LinkDown:     return "link-down";
            case affa::Result::Cancelled:    return "cancelled";
            // 11 was Busy — the sendBlocking() re-entry refusal. Both are gone
            // from the library, and the number is deliberately not reused, so a
            // log line that once said "busy" now says "?" rather than a lie.
            case affa::Result::Aborted:      return "aborted";
        }
        return "?";
    }
}
