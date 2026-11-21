#include "schema-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCHEMA)

static int dyn_fm_regex(JSContext* ctx, const char* s, size_t n)
{
    char err[64];
    int len = 0;
    char* z = js_malloc(ctx, n + 1);
    uint8_t* bc;
    if (!z)
        return -1;
    memcpy(z, s, n);
    z[n] = 0;
    bc = lre_compile(&len, err, sizeof err, z, n, LRE_FLAG_UNICODE, ctx);
    js_free(ctx, z);
    if (!bc)
        return 0;
    js_free(ctx, bc);
    return 1;
}

int dyn_fm_builtin_engine(JSContext* ctx, const char* fmt, const char* s, size_t n)
{
    if (!strcmp(fmt, "regex"))
        return dyn_fm_regex(ctx, s, n);
    return 1;
}

#endif
