#include "dyna-nat.h"

#include <stdio.h>

#if defined(CONFIG_NATIVE_MODULES)

static _Atomic(uint64_t) dyn_net_handler_throws;

uint64_t dyn_net_handler_throw_count(void)
{
    return dyn_net_handler_throws;
}

void dyn_net_handler_result(JSContext* ctx, JSValue r, int kind)
{
    if (!JS_IsException(r)) {
        JS_FreeValue(ctx, r);
        return;
    }
    if (JS_HasUncatchableException(ctx)) {
        dyn_exc_forward(ctx);
        return;
    }
    dyn_net_handler_throws++;
    dyn_exc_drop(ctx);
}

#endif
