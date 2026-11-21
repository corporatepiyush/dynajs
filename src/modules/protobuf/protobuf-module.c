#include "protobuf-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_PROTOBUF)

const char* const pb_type_names[PB_T_COUNT] = {
    "int32",
    "int64",
    "uint32",
    "uint64",
    "sint32",
    "sint64",
    "fixed32",
    "sfixed32",
    "fixed64",
    "sfixed64",
    "float",
    "double",
    "bool",
    "string",
    "bytes",
    "message",
    "enum",
};

int pb_wire_of_type(int t)
{
    switch (t) {
    case PB_T_INT32:
    case PB_T_INT64:
    case PB_T_UINT32:
    case PB_T_UINT64:
    case PB_T_SINT32:
    case PB_T_SINT64:
    case PB_T_BOOL:
    case PB_T_ENUM:
        return 0;
    case PB_T_FIXED64:
    case PB_T_SFIXED64:
    case PB_T_DOUBLE:
        return 1;
    case PB_T_STRING:
    case PB_T_BYTES:
    case PB_T_MESSAGE:
        return 2;
    case PB_T_FIXED32:
    case PB_T_SFIXED32:
    case PB_T_FLOAT:
        return 5;
    default:
        return -1;
    }
}

static JSValue pb_new_bytes(JSContext* ctx, const uint8_t* p, size_t n)
{
    static const uint8_t zero = 0;
    JSValueConst ta[3];
    JSValue ab, out;
    ab = JS_NewArrayBufferCopy(ctx, n ? p : &zero, n);
    if (JS_IsException(ab))
        return ab;
    ta[0] = ab;
    ta[1] = JS_UNDEFINED;
    ta[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return out;
}

static JSValue pb_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    pb_schema_t s;
    pb_enc_t e;
    JSValue out;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "Proto.encode(value, schema): a schema "
                                      "is required");
    if (pb_schema_parse(ctx, argv[1], &s, 0) < 0)
        return JS_EXCEPTION;
    memset(&e, 0, sizeof e);
    e.ctx = ctx;
    if (pb_enc_message(&e, argv[0], &s) < 0 || e.w.oom) {
        if (e.w.oom)
            JS_ThrowOutOfMemory(ctx);
        free(e.w.p);
        pb_schema_free(ctx, &s);
        return JS_EXCEPTION;
    }
    out = pb_new_bytes(ctx, e.w.p, e.w.n);
    free(e.w.p);
    pb_schema_free(ctx, &s);
    return out;
}

static JSValue dyn_proto_namespace(JSContext* ctx)
{
    JSValue proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return proto;
    if (JS_DefinePropertyValueStr(ctx, proto, "encode",
            JS_NewCFunction(ctx, pb_encode, "encode", 2), JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, proto, "decode",
               JS_NewCFunction(ctx, pb_decode, "decode", 2), JS_PROP_C_W_E)
            < 0) {
        JS_FreeValue(ctx, proto);
        return JS_EXCEPTION;
    }
    return proto;
}

static int dyn_proto_init_module(JSContext* ctx, JSModuleDef* m)
{
    JSValue proto = dyn_proto_namespace(ctx);
    if (JS_IsException(proto))
        return -1;
    return JS_SetModuleExport(ctx, m, "Proto", proto);
}

int js_nat_init_protobuf(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:protobuf", dyn_proto_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExport(ctx, m, "Proto");
}

#endif
