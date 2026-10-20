#include "TaskRegistry.h"
#include "TaskAge.h"

namespace App
{
    namespace Tasks
    {
        namespace
        {
            TaskEntry    g_e[MAX];
            // Published after the entry is filled, so a reader never sees a
            // half-written one. add() is the only writer, under the mux.
            volatile int g_n = 0;
            portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
        }

        int add(const char* name, bool critical)
        {
            const uint32_t now = millis();
            int id = -1;
            portENTER_CRITICAL(&g_mux);
            if (g_n < MAX) {
                id = g_n;
                TaskEntry& e = g_e[id];
                e.name = name;
                e.critical = critical;
                e.lastAliveMs = now;
                g_n = id + 1;
            }
            portEXIT_CRITICAL(&g_mux);
            return id;
        }

        void setHandle(int id, TaskHandle_t h)
        {
            if (id >= 0 && id < MAX) g_e[id].handle = h;
        }

        void beat(int id, uint32_t busyUs)
        {
            if (id < 0 || id >= MAX) return;
            TaskEntry& e = g_e[id];
            const uint32_t req = e.winReq;
            if (req != e.winSeen) {
                e.winSeen = req;
                e.busyMaxUs = 0;
            }
            if (busyUs > e.busyMaxUs) e.busyMaxUs = busyUs;
            e.wakes = e.wakes + 1;
            e.lastAliveMs = millis();
        }

        int count() { return g_n; }

        const TaskEntry* at(int i) { return (i >= 0 && i < g_n) ? &g_e[i] : nullptr; }

        bool criticalAlive(uint32_t nowMs, uint32_t maxAgeMs, const char** late)
        {
            // The rule itself is in TaskAge.h, host-tested.
            const int i = firstLateCritical(g_e, g_n, nowMs, maxAgeMs);
            if (i < 0) return true;
            if (late) *late = g_e[i].name;
            return false;
        }

        void newWindow()
        {
            const int n = g_n;
            for (int i = 0; i < n; ++i) g_e[i].winReq = g_e[i].winReq + 1;
        }
    }
}
