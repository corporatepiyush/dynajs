#include "uuid-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_UUID)

static int dyn_uuid_opt_as(JSContext* ctx, JSValueConst opts, const char* who)
{
    JSValue as_val;
    int out = 0;

    if (!(JS_IsObject(opts)))
        return 0;
    as_val = JS_GetPropertyStr(ctx, opts, "as");
    if (JS_IsException(as_val))
        return -1;
    if (JS_IsString(as_val)) {
        size_t n;
        const char* a = JS_ToCStringLen(ctx, &n, as_val);
        if (!a) {
            JS_FreeValue(ctx, as_val);
            return -1;
        }
        if (strcmp(a, "bytes") == 0)
            out = 1;
        else if (strcmp(a, "string") != 0) {
            JS_ThrowTypeError(ctx, "%s: as must be \"string\" or \"bytes\"", who);
            out = -1;
        }
        JS_FreeCString(ctx, a);
    } else if (!JS_IsUndefined(as_val)) {
        JS_ThrowTypeError(ctx, "%s: as must be \"string\" or \"bytes\"", who);
        out = -1;
    }
    JS_FreeValue(ctx, as_val);
    return out;
}

JSValue dyn_uuid_v4(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    static const char* const allowed[] = { "as" };
    uint8_t b[16];
    char s[DYN_UUID_STRLEN];
    int as_bytes;

    (void)this_val;
    if (dyn_uuid_opts_check(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
            allowed, 1, "v4(opts)"))
        return JS_EXCEPTION;
    as_bytes = dyn_uuid_opt_as(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
        "v4(opts)");
    if (as_bytes < 0)
        return JS_EXCEPTION;

    if (dyn_os_entropy(b, sizeof(b)) < 0)
        return JS_ThrowInternalError(ctx, "dyna:uuid: OS entropy unavailable");
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
    if (as_bytes)
        return dyn_uuid_new_u8array(ctx, b, 16);
    dyn_uuid_format(b, s);
    return JS_NewStringLen(ctx, s, DYN_UUID_STRLEN);
}

JSValue dyn_uuid_v7(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    static const char* const allowed[] = { "as" };
    uint8_t b[16];
    char s[DYN_UUID_STRLEN];
    struct timespec ts;
    uint64_t ms, ctr;
    int as_bytes;

    (void)this_val;
    if (dyn_uuid_opts_check(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
            allowed, 1, "v7(opts)"))
        return JS_EXCEPTION;
    as_bytes = dyn_uuid_opt_as(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
        "v7(opts)");
    if (as_bytes < 0)
        return JS_EXCEPTION;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return JS_ThrowInternalError(ctx, "dyna:uuid: clock_gettime failed");
    ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;

    if (dyn_os_entropy(b, sizeof(b)) < 0)
        return JS_ThrowInternalError(ctx, "dyna:uuid: OS entropy unavailable");

    {
        static _Atomic uint64_t v7_mono;
        uint64_t cur = atomic_load_explicit(&v7_mono, memory_order_relaxed);
        uint64_t fresh = ((((uint64_t)b[0] & 0xF0) << 4) | b[1]) & 0xFFF;
        uint64_t next;
        for (;;) {
            uint64_t cur_ms = cur >> 12;
            if (ms < cur_ms)
                ms = cur_ms;
            if (cur_ms == ms) {
                if ((cur & 0xFFF) == 0xFFF) {
                    ms += 1;
                    next = (ms << 12) | fresh;
                } else {
                    next = cur + 1;
                }
            } else {
                next = (ms << 12) | fresh;
            }
            if (atomic_compare_exchange_weak_explicit(
                    &v7_mono, &cur, next,
                    memory_order_relaxed, memory_order_relaxed))
                break;
        }
        ms = next >> 12;
        ctr = next & 0xFFF;
    }

    b[0] = (uint8_t)(ms >> 40);
    b[1] = (uint8_t)(ms >> 32);
    b[2] = (uint8_t)(ms >> 24);
    b[3] = (uint8_t)(ms >> 16);
    b[4] = (uint8_t)(ms >> 8);
    b[5] = (uint8_t)ms;
    b[6] = (uint8_t)((ctr >> 8) | 0x70);
    b[7] = (uint8_t)ctr;
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
    if (as_bytes)
        return dyn_uuid_new_u8array(ctx, b, 16);
    dyn_uuid_format(b, s);
    return JS_NewStringLen(ctx, s, DYN_UUID_STRLEN);
}

JSValue dyn_uuid_v8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    static const char* const allowed[] = { "fill" };
    uint8_t b[16];
    uint8_t* p;
    size_t n;
    JSValue fv;
    char s[DYN_UUID_STRLEN];

    (void)this_val;
    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx,
            "v8(opts): opts with {fill} is required");
    if (dyn_uuid_opts_check(ctx, argv[0], allowed, 1, "v8(opts)"))
        return JS_EXCEPTION;
    fv = JS_GetPropertyStr(ctx, argv[0], "fill");
    if (JS_IsException(fv))
        return JS_EXCEPTION;
    if (JS_IsUndefined(fv)) {
        JS_FreeValue(ctx, fv);
        return JS_ThrowTypeError(ctx, "v8(opts): fill is required");
    }
    if (dyn_uuid_view(ctx, fv, &p, &n)) {
        JS_FreeValue(ctx, fv);
        return JS_EXCEPTION;
    }
    if (n != 16) {
        JS_FreeValue(ctx, fv);
        return JS_ThrowRangeError(ctx,
            "v8(opts): fill must be exactly 16 bytes");
    }
    memcpy(b, p, 16);
    JS_FreeValue(ctx, fv);
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x80);
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
    dyn_uuid_format(b, s);
    return JS_NewStringLen(ctx, s, DYN_UUID_STRLEN);
}

static int dyn_uuid_namespace(JSContext* ctx, JSValueConst v, uint8_t ns[16])
{
    if (JS_IsString(v)) {
        size_t len;
        const char* s = JS_ToCStringLen(ctx, &len, v);
        int r;
        if (!s)
            return -1;
        r = dyn_uuid_parse_bytes(s, len, ns);
        JS_FreeCString(ctx, s);
        if (r) {
            JS_ThrowSyntaxError(ctx, "v3/v5: invalid namespace UUID");
            return -1;
        }
        return 0;
    }
    {
        uint8_t* p;
        size_t n;
        if (dyn_uuid_view(ctx, v, &p, &n))
            return -1;
        if (n != 16) {
            JS_ThrowTypeError(ctx, "v3/v5: namespace must be a UUID string or 16-byte view");
            return -1;
        }
        memcpy(ns, p, 16);
        return 0;
    }
}

JSValue dyn_uuid_named(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    uint8_t ns[16], b[16], digest[20];
    const uint8_t* name = NULL;
    const char* owned = NULL;
    size_t namelen = 0;
    uint8_t* msg;
    char s[DYN_UUID_STRLEN];
    (void)this_val;
    (void)argc;

    if (dyn_uuid_namespace(ctx, argv[0], ns))
        return JS_EXCEPTION;
    if (JS_IsString(argv[1])) {
        owned = JS_ToCStringLen(ctx, &namelen, argv[1]);
        if (!owned)
            return JS_EXCEPTION;
        name = (const uint8_t*)owned;
    } else {
        uint8_t* p;
        size_t n;
        if (dyn_uuid_view(ctx, argv[1], &p, &n))
            return JS_EXCEPTION;
        name = p;
        namelen = n;
    }

    msg = (uint8_t*)malloc(16 + namelen);
    if (!msg) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(msg, ns, 16);
    if (namelen)
        memcpy(msg + 16, name, namelen);
    if (magic == 5) {
        dyn_sha1(msg, 16 + namelen, digest);
        memcpy(b, digest, 16);
    } else {
        dyn_md5(msg, 16 + namelen, b);
    }
    free(msg);
    if (owned)
        JS_FreeCString(ctx, owned);

    b[6] = (uint8_t)((b[6] & 0x0f) | (magic << 4));
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
    dyn_uuid_format(b, s);
    return JS_NewStringLen(ctx, s, DYN_UUID_STRLEN);
}

#endif
