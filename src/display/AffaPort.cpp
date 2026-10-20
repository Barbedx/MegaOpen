#include "AffaPort.h"
#include <string.h>

namespace MegaOpen
{
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
