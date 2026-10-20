#pragma once
#include <stdint.h>

// What this firmware can say to a RAV4-profile head unit, and how sure it is.
//
// The INVENTORY: every key code and message, with its confidence from
// docs/raise-protocol.md.
//
// Pure data, no Arduino, so the native build checks the tables.
namespace Canbox
{
    // The confidence legend of docs/raise-protocol.md, in code.
    enum class Conf : uint8_t {
        Confirmed = 0,  // observed working on THIS head unit or THIS car
        High,           // official spec, or two independent primaries agreeing
        Medium,         // one primary only
        Low,            // name known, layout not recovered
        Conflict,       // primaries disagree and it is NOT resolved
    };
    const char* confName(Conf c);

    struct KeyCode {
        uint8_t     code;
        const char* name;
        Conf        conf;
        const char* note;      // "" when there is nothing worth saying
    };
    uint8_t        keyCodeCount();
    const KeyCode& keyCodeAt(uint8_t i);
    const char*    keyCodeName(uint8_t code);   // "?" when not in the table

    // Per Rav4Emitter message index.
    struct MsgInfo {
        Conf        conf;
        const char* note;
    };
    const MsgInfo& msgInfo(uint8_t msgIndex);
}
