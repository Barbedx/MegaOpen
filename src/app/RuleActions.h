#pragma once
#include "../vehicle/Rules.h"

namespace App
{
    namespace RuleActions
    {
        // One of a rule's actions, performed from the car task: each goes where
        // Keys::effectRoute() says, as a post to the task that owns it (the keys
        // task's bus, the hu task's lanes, the ui task's requests). The popup is not
        // here: the car task queues it itself (one pending, when the glass is free).
        // CarTask's Setup::perform.
        void perform(const Vehicle::RuleAction& a);
    }
}
