#include "WireProto.h"

namespace WireProto
{
    namespace
    {
        constexpr int MAX_LINKS = 3;
        WireLink* g_links[MAX_LINKS] = {};
        int g_linkCount = 0;
        WireLink::CommandCb g_cmd = nullptr;
        void* g_cmdCtx = nullptr;
    }

    void addLink(WireLink* link)
    {
        if (link && g_linkCount < MAX_LINKS)
            g_links[g_linkCount++] = link;
    }

    void onCommand(WireLink::CommandCb cb, void* ctx)
    {
        g_cmd = cb;
        g_cmdCtx = ctx;
    }

    void dispatchCommand(const char* line)
    {
        if (g_cmd) g_cmd(line, g_cmdCtx);
    }

    int buildFrameLine(char* buf, int size, const char* tag,
                       uint32_t id, const uint8_t* data, uint8_t len)
    {
        int p = snprintf(buf, size, "%s %03X", tag, (unsigned)id);
        for (int i = 0; i < len && p < size - 4; i++)
            p += snprintf(buf + p, size - p, " %02X", data[i]);
        return p;
    }

    void emitLine(const char* line)
    {
        for (int i = 0; i < g_linkCount; i++)
            g_links[i]->emitLine(line);
    }

    void emitTx(uint32_t id, const uint8_t* data, uint8_t len)
    {
        char buf[64];
        buildFrameLine(buf, sizeof(buf), TAG_TX, id, data, len);
        emitLine(buf);
    }
}
