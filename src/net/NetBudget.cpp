#include "NetBudget.h"
#include <esp_heap_caps.h>
#include "../link/AppLink.h"

namespace NetBudget
{
    bool admit(Sample* out)
    {
        constexpr uint32_t CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        Sample s;
        s.path = AppLink::netFirst() ? Path::App : Path::Wifi;
        s.need = needFree(s.path);
        s.free = (uint32_t)heap_caps_get_free_size(CAPS);
        // The walk only when the free size passes: a held job asks every pass.
        s.big  = s.free >= s.need ? (uint32_t)heap_caps_get_largest_free_block(CAPS) : 0;
        if (out) *out = s;
        return verdict(s.path, s.free, s.big);
    }
}
