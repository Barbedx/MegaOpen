#include "Supervisor.h"
#include "TaskPlan.h"
#include "TaskRegistry.h"
#include "../Features.h"
#include "../utils/LogBus.h"

namespace App
{
    namespace Supervisor
    {
        namespace
        {
            // Off until begin(): setup() runs on the loop for seconds before
            // its first beat.
            volatile bool s_on = false;
            uint32_t s_checkedAt = 0;
            // Per registry id, written only here (the logbus task).
            uint32_t s_limit[Tasks::MAX] = {};
            bool     s_limitKnown[Tasks::MAX] = {};
            bool     s_late[Tasks::MAX] = {};
            uint32_t s_staleAt[Tasks::MAX] = {};   // its lastAliveMs when it went late
        }

        void begin()
        {
#if FEATURE_TASK_WDT
            // Arduino's loop task feeds it around every loop() (its own
            // loopTaskWDTEnabled); the loop wakes every 2 ms at most.
            enableLoopWDT();
            LOGI("task", "loop: task watchdog on");
#endif
            s_on = true;
        }

        void tick(uint32_t now)
        {
            if (!s_on || (uint32_t)(now - s_checkedAt) < CHECK_MS) return;
            s_checkedAt = now;
            const int n = Tasks::count();
            for (int i = 0; i < n && i < Tasks::MAX; ++i) {
                const TaskEntry* e = Tasks::at(i);
                if (!e || !e->name) continue;
                if (!s_limitKnown[i]) {
                    s_limit[i] = Plan::lateLimitMs(e->name);
                    s_limitKnown[i] = true;
                }
                const uint32_t limit = s_limit[i];
                if (!limit) continue;
                const uint32_t alive = e->lastAliveMs;
                // Signed: the task may have beaten after `now` was read.
                const int32_t age = (int32_t)(now - alive);
                if (!s_late[i]) {
                    if (age > (int32_t)limit) {
                        s_late[i] = true;
                        s_staleAt[i] = alive;
                        LOGW("sup", "late task=%s age=%u limit=%u", e->name, (unsigned)age, (unsigned)limit);
                    }
                } else if (alive != s_staleAt[i]) {
                    s_late[i] = false;
                    LOGI("sup", "task=%s back after %u ms", e->name, (unsigned)(alive - s_staleAt[i]));
                }
            }
        }
    }
}
