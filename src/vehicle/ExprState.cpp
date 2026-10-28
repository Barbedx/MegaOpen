#include "ExprState.h"
#include <string.h>

namespace Vehicle
{
    int exprResolve(const char* name, size_t len, void*)
    {
        char b[40];
        if (len >= sizeof(b)) return -1;
        memcpy(b, name, len);
        b[len] = 0;
        const uint8_t s = sigByName(b);
        return s < SIG_COUNT ? s : -1;
    }

    bool exprRead(uint8_t id, Expr::Num& v, uint32_t& ageMs, void* ctx)
    {
        const ExprCtx* c = static_cast<const ExprCtx*>(ctx);
        if (!c || !c->st || id >= SIG_COUNT || !c->st->has(id)) return false;
        v = Expr::fromRaw(c->st->get(id), meta(id).decimals);
        ageMs = c->st->ageMs(id, c->nowMs);
        return true;
    }
}
