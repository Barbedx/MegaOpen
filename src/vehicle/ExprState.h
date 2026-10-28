#pragma once
#include "Expr.h"
#include "VehicleState.h"

// The formula language over the car's signals (Expr.h): a name in a formula is
// a signal's name (Signals.cpp), its value is State's in the signal's own
// units (speed in km/h, not x100), its age is State's.
namespace Vehicle
{
    struct ExprCtx {
        const State* st;
        uint32_t     nowMs;
    };
    // Expr::ResolveFn: ctx unused.
    int  exprResolve(const char* name, size_t len, void* ctx);
    // Expr::ReadFn: ctx is an ExprCtx.
    bool exprRead(uint8_t id, Expr::Num& v, uint32_t& ageMs, void* ctx);
}
