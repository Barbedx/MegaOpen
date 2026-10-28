#pragma once
#include "Protocol.h"

// Raise protocol 112, the Toyota RAV4 profile (docs/raise-protocol.md): the
// field list (RaiseTargets), the message table and emitter (Rav4Emitter),
// the framing (RaiseFramer), and its settings, the Tuning block (raise.*).
namespace Canbox
{
    const Protocol& raise112();
}
