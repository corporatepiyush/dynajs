#include "dyna-nat.h"

#include <stdlib.h>

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

static int dyn_net_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_tcp_register(ctx, m) < 0)
        return -1;
    if (dyn_ratelimit_register(ctx, m) < 0)
        return -1;
    return 0;
}

int js_nat_init_net(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:net", dyn_net_init_module);
    if (!m)
        return -1;
    dyn_tcp_add_exports(ctx, m);
    dyn_ratelimit_add_exports(ctx, m);
    return 0;
}

#endif
