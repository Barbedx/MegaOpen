#include "KeyAction.h"
#include "../utils/UiList.h"

namespace Keys
{
    // Ids are stored in NVS and the API; names are what /api/catalog shows.
    // The phases are matrix positions, never offered as an action, so unnamed.
    static const UiName kNames[] = {
        {"None",      "—"},
        {"Load",      "Load"},
        {"SrcNext",   "Джерело ▶"},
        {"SrcPrev",   "Джерело ◀"},
        {"VolUp",     "Гучність +"},
        {"VolDown",   "Гучність −"},
        {"Mute",      "Тиша"},
        {"EncCw",     "Крок ▶"},
        {"EncCcw",    "Крок ◀"},
        {"Ok",        "OK"},
        {"Phone",     "Телефон"},
        {"Voice",     "Голос"},
        {"Mode",      "MODE"},
        {"Radio",     "RADIO"},
        {"EncPhaseA", ""},
        {"EncPhaseB", ""},
        {"EncPhaseC", ""},
    };
    static_assert(sizeof(kNames) / sizeof(kNames[0]) == (size_t)Action::_Count,
                  "an Action has no row in kNames");

    const char* name(Action a) {
        uint8_t i = static_cast<uint8_t>(a);
        if (i >= static_cast<uint8_t>(Action::_Count)) return "?";
        return kNames[i].id;
    }

    const char* title(Action a) {
        uint8_t i = static_cast<uint8_t>(a);
        return i < static_cast<uint8_t>(Action::_Count) ? kNames[i].name : "";
    }

    uint8_t classOf(Action a) {
        switch (a) {
            case Action::VolUp:
            case Action::VolDown:
            case Action::Mute:      return CLASS_VOLUME;

            case Action::Load:
            case Action::SrcNext:
            case Action::SrcPrev:
            case Action::EncCw:
            case Action::EncCcw:
            case Action::Ok:        return CLASS_TRANSPORT;

            case Action::Phone:
            case Action::Voice:     return CLASS_PHONE;

            case Action::Mode:
            case Action::Radio:     return CLASS_UI;

            // None and the three encoder phases. A phase is a matrix POSITION
            // that EncoderDecoder consumes; it is never dispatched as an event,
            // and belonging to no class means that if one ever were, no sink
            // would act on it.
            default:                return CLASS_NONE;
        }
    }

    // Row i is the class 1 << i.
    static const UiName kClasses[CLASS_BITS] = {
        {"transport", "треки"},
        {"volume",    "гучність"},
        {"phone",     "телефон"},
        {"ui",        "меню"},
    };

    static int classIndex(uint8_t bit) {
        for (uint8_t i = 0; i < CLASS_BITS; ++i) if (bit == (1u << i)) return i;
        return -1;
    }

    const char* classTitle(uint8_t bit) {
        const int i = classIndex(bit);
        return i < 0 ? "?" : kClasses[i].name;
    }

    Action fromName(const char* s) {
        if (!s) return Action::None;
        for (uint8_t i = 0; i < static_cast<uint8_t>(Action::_Count); ++i) {
            const char* n = kNames[i].id;
            const char* p = s;
            while (*n && *p && *n == *p) { ++n; ++p; }
            if (!*n && !*p) return static_cast<Action>(i);
        }
        return Action::None;
    }
}
