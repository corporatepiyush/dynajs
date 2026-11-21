#include "uuid-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_UUID)

const char dyn_uuid_hex[] = "0123456789abcdef";

JSValue dyn_uuid_new_u8array(JSContext* ctx, const uint8_t* data, size_t len)
{
    JSValue ab, out;
    JSValueConst ta_args[3];

    ab = JS_NewArrayBufferCopy(ctx, data, len);
    if (JS_IsException(ab))
        return ab;
    ta_args[0] = ab;
    ta_args[1] = JS_UNDEFINED;
    ta_args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta_args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return out;
}

int dyn_uuid_opts_check(JSContext* ctx, JSValueConst opts,
    const char* const* allowed, size_t n_allowed,
    const char* who)
{
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i;
    size_t j;

    if (JS_IsUndefined(opts))
        return 0;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "%s: opts must be an object", who);
        return -1;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, opts,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < len; i++) {
        size_t name_len = 0;
        const char* name = JS_AtomBorrowASCII(ctx, &name_len, tab[i].atom);
        int ok = 0;

        if (name) {
            for (j = 0; j < n_allowed; j++) {
                if (strcmp(name, allowed[j]) == 0) {
                    ok = 1;
                    break;
                }
            }
        }
        if (!ok) {
            const char* shown = JS_AtomToCString(ctx, tab[i].atom);
            JS_ThrowTypeError(ctx, "%s: unknown option \"%s\"", who,
                shown ? shown : "?");
            if (shown)
                JS_FreeCString(ctx, shown);
            JS_FreePropertyEnum(ctx, tab, len);
            return -1;
        }
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return 0;
}

static JSValue dyn_uuid_validate(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* str;
    size_t len;
    uint8_t b[16];
    int ok;
    (void)this_val;
    (void)argc;

    if (!JS_IsString(argv[0]))
        return JS_FALSE;
    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    ok = (dyn_uuid_parse_bytes(str, len, b) == 0);
    JS_FreeCString(ctx, str);
    return JS_NewBool(ctx, ok);
}

static JSValue dyn_uuid_version(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* str;
    size_t len;
    uint8_t b[16];
    int r;
    (void)this_val;
    (void)argc;

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    r = dyn_uuid_parse_bytes(str, len, b);
    JS_FreeCString(ctx, str);
    if (r)
        return JS_ThrowSyntaxError(ctx, "version: invalid UUID string");
    return JS_NewInt32(ctx, b[6] >> 4);
}

static const JSCFunctionListEntry dyn_uuid_funcs[] = {
    JS_CFUNC_DEF("NanoID", 0, dyn_nanoid),
    JS_CFUNC_DEF("NanoIDAlphabet", 1, dyn_nanoid_alphabet),
    JS_CFUNC_DEF("ULID", 2, dyn_ulid),
    JS_CFUNC_DEF("ULIDTime", 1, dyn_ulid_time),
    JS_CFUNC_DEF("v4", 1, dyn_uuid_v4),
    JS_CFUNC_DEF("v7", 1, dyn_uuid_v7),
    JS_CFUNC_DEF("v8", 1, dyn_uuid_v8),
    JS_CFUNC_DEF("compare", 2, dyn_uuid_compare),
    JS_CFUNC_MAGIC_DEF("v3", 2, dyn_uuid_named, 3),
    JS_CFUNC_MAGIC_DEF("v5", 2, dyn_uuid_named, 5),
    JS_CFUNC_DEF("parse", 1, dyn_uuid_parse),
    JS_CFUNC_DEF("validate", 1, dyn_uuid_validate),
    JS_CFUNC_DEF("version", 1, dyn_uuid_version),
    JS_CFUNC_DEF("variant", 1, dyn_uuid_variant),
    JS_CFUNC_DEF("bytes", 1, dyn_uuid_bytes),
    JS_CFUNC_DEF("fromBytes", 1, dyn_uuid_from_bytes),
    JS_PROP_STRING_DEF("NIL", "00000000-0000-0000-0000-000000000000", 0),
    JS_PROP_STRING_DEF("MAX", "ffffffff-ffff-ffff-ffff-ffffffffffff", 0),
    JS_PROP_STRING_DEF("NAMESPACE_DNS", "6ba7b810-9dad-11d1-80b4-00c04fd430c8", 0),
    JS_PROP_STRING_DEF("NAMESPACE_URL", "6ba7b811-9dad-11d1-80b4-00c04fd430c8", 0),
    JS_PROP_STRING_DEF("NAMESPACE_OID", "6ba7b812-9dad-11d1-80b4-00c04fd430c8", 0),
    JS_PROP_STRING_DEF("NAMESPACE_X500", "6ba7b814-9dad-11d1-80b4-00c04fd430c8", 0),
};

static int dyn_uuid_init_module(JSContext* ctx, JSModuleDef* m)
{
    return JS_SetModuleExportList(ctx, m, dyn_uuid_funcs, countof(dyn_uuid_funcs));
}

int js_nat_init_uuid(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:uuid", dyn_uuid_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_uuid_funcs, countof(dyn_uuid_funcs));
}

#endif
