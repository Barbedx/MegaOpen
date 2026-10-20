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
        // Anything else, including a stored "updatelist_menu" (the library has
        // one UpdateList encoding since 2.0), is an UpdateList.
        return new affa::UpdateListDisplay(link, clock);
    }
}
