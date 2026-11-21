#ifndef NETIP_INTERNAL_H
#define NETIP_INTERNAL_H

#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NETIP)

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/dyn-netipk.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

static inline const char* netip_cstr(JSContext* ctx, size_t* plen, JSValueConst v)
{
    const uint8_t* b = JS_GetNarrowStringBytes(ctx, v, plen);
    return b ? (const char*)b : JS_ToCStringLen(ctx, plen, v);
}

static inline void netip_cfree(JSContext* ctx, JSValueConst v, const char* s)
{
    if (!JS_GetNarrowStringBytes(ctx, v, NULL))
        JS_FreeCString(ctx, s);
}

static inline const char* netip_arg_str(JSContext* ctx, int argc, JSValueConst* argv,
    size_t* plen)
{
    if (argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx, "dyna:netip: argument must be a string");
        return NULL;
    }
    return netip_cstr(ctx, plen, argv[0]);
}

JSValue dyn_netip_parse_addr(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_netip_parse_prefix(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_netip_contains(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_netip_classify(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

typedef struct {
    NetAddr net;
    int bits;
} dyn_prefix_t;

extern JSClassID dyn_prefix_class_id;

JSValue dyn_prefix_contains(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#endif

#endif
