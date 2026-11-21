#include "libc-internal.h"

typedef struct DomEx {
    JSValue name, message;
} DomEx;

static JSClassID domex_cid;

static const char* const domex_names[] = {
    "IndexSizeError", "DOMStringSizeError", "HierarchyRequestError", "WrongDocumentError",
    "InvalidCharacterError", "NoDataAllowedError", "NoModificationAllowedError", "NotFoundError",
    "NotSupportedError", "InUseAttributeError", "InvalidStateError", "SyntaxError",
    "InvalidModificationError", "NamespaceError", "InvalidAccessError", "ValidationError",
    "TypeMismatchError", "SecurityError", "NetworkError", "AbortError", "URLMismatchError",
    "QuotaExceededError", "TimeoutError", "InvalidNodeTypeError", "DataCloneError"
};

static const char* const domex_consts[] = {
    "INDEX_SIZE_ERR", "DOMSTRING_SIZE_ERR", "HIERARCHY_REQUEST_ERR", "WRONG_DOCUMENT_ERR",
    "INVALID_CHARACTER_ERR", "NO_DATA_ALLOWED_ERR", "NO_MODIFICATION_ALLOWED_ERR", "NOT_FOUND_ERR",
    "NOT_SUPPORTED_ERR", "INUSE_ATTRIBUTE_ERR", "INVALID_STATE_ERR", "SYNTAX_ERR",
    "INVALID_MODIFICATION_ERR", "NAMESPACE_ERR", "INVALID_ACCESS_ERR", "VALIDATION_ERR",
    "TYPE_MISMATCH_ERR", "SECURITY_ERR", "NETWORK_ERR", "ABORT_ERR", "URL_MISMATCH_ERR",
    "QUOTA_EXCEEDED_ERR", "TIMEOUT_ERR", "INVALID_NODE_TYPE_ERR", "DATA_CLONE_ERR"
};

static void domex_fin(JSRuntime* rt, JSValue v)
{
    DomEx* d = JS_GetOpaque(v, domex_cid);
    if (!d)
        return;
    JS_FreeValueRT(rt, d->name);
    JS_FreeValueRT(rt, d->message);
    js_free_rt(rt, d);
}

static void domex_mark(JSRuntime* rt, JSValueConst v, JS_MarkFunc* mf)
{
    DomEx* d = JS_GetOpaque(v, domex_cid);
    if (!d)
        return;
    JS_MarkValue(rt, d->name, mf);
    JS_MarkValue(rt, d->message, mf);
}

static JSClassDef domex_class = { "DOMException", .finalizer = domex_fin, .gc_mark = domex_mark };

static JSValue domex_make(JSContext* ctx, JSValueConst proto, JSValue message, JSValue name)
{
    JSValue obj = JS_IsUndefined(proto) ? JS_NewObjectClass(ctx, (int)domex_cid) : JS_NewObjectProtoClass(ctx, proto, domex_cid);
    DomEx* d;
    if (JS_IsException(obj))
        goto fail;
    d = js_mallocz(ctx, sizeof(*d));
    if (!d) {
        JS_FreeValue(ctx, obj);
        goto fail;
    }
    d->name = name;
    d->message = message;
    JS_SetOpaque(obj, d);
    return obj;
fail:
    JS_FreeValue(ctx, message);
    JS_FreeValue(ctx, name);
    return JS_EXCEPTION;
}

static JSValue domex_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    JSValue proto, msg, name;
    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "Constructor DOMException requires 'new'");
    msg = (argc > 0 && !JS_IsUndefined(argv[0])) ? JS_ToString(ctx, argv[0]) : JS_NewString(ctx, "");
    if (JS_IsException(msg))
        return msg;
    name = (argc > 1 && !JS_IsUndefined(argv[1])) ? JS_ToString(ctx, argv[1]) : JS_NewString(ctx, "Error");
    if (JS_IsException(name)) {
        JS_FreeValue(ctx, msg);
        return name;
    }
    proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) {
        JS_FreeValue(ctx, msg);
        JS_FreeValue(ctx, name);
        return proto;
    }
    if (!JS_IsObject(proto)) {
        JS_FreeValue(ctx, proto);
        proto = JS_GetClassProto(ctx, domex_cid);
    }
    {
        JSValue r = domex_make(ctx, proto, msg, name);
        JS_FreeValue(ctx, proto);
        return r;
    }
}

static DomEx* domex_this(JSContext* ctx, JSValueConst v)
{
    DomEx* d = JS_GetOpaque(v, domex_cid);
    if (!d)
        JS_ThrowTypeError(ctx, "Illegal invocation: not a DOMException");
    return d;
}

static int domex_code_of(JSContext* ctx, JSValueConst name)
{
    const char* s = JS_ToCString(ctx, name);
    int i, code = 0;
    if (!s)
        return 0;
    for (i = 0; i < (int)countof(domex_names); i++)
        if (!strcmp(s, domex_names[i])) {
            code = i + 1;
            break;
        }
    JS_FreeCString(ctx, s);
    return code;
}

static JSValue domex_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    DomEx* d = domex_this(ctx, this_val);
    if (!d)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_DupValue(ctx, d->name);
    if (magic == 1)
        return JS_DupValue(ctx, d->message);
    return JS_NewInt32(ctx, domex_code_of(ctx, d->name));
}

static const JSCFunctionListEntry domex_proto_funcs[] = {
    JS_CGETSET_MAGIC_DEF("name", domex_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("message", domex_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("code", domex_get, NULL, 2),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "DOMException", JS_PROP_CONFIGURABLE),
};

static int domex_def_consts(JSContext* ctx, JSValueConst o)
{
    int i;
    for (i = 0; i < (int)countof(domex_consts); i++)
        if (JS_DefinePropertyValueStr(ctx, o, domex_consts[i], JS_NewInt32(ctx, i + 1), JS_PROP_ENUMERABLE) < 0)
            return -1;
    return 0;
}

static int domex_ensure(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue proto, ep, g;
    int have;
    JS_NewClassID(&domex_cid);
    if (!JS_IsRegisteredClass(rt, domex_cid) && JS_NewClass(rt, domex_cid, &domex_class) < 0)
        return -1;
    proto = JS_GetClassProto(ctx, domex_cid);
    have = JS_IsObject(proto);
    JS_FreeValue(ctx, proto);
    if (have)
        return 0;
    g = JS_GetGlobalObject(ctx);
    ep = JS_GetPropertyStr(ctx, g, "Error");
    JS_FreeValue(ctx, g);
    proto = JS_IsException(ep) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, ep, "prototype");
    JS_FreeValue(ctx, ep);
    if (JS_IsException(proto))
        return -1;
    ep = proto;
    proto = JS_NewObjectProto(ctx, ep);
    JS_FreeValue(ctx, ep);
    if (JS_IsException(proto))
        return -1;
    if (JS_SetPropertyFunctionList(ctx, proto, domex_proto_funcs, countof(domex_proto_funcs)) < 0
        || domex_def_consts(ctx, proto) < 0) {
        JS_FreeValue(ctx, proto);
        return -1;
    }
    g = JS_NewCFunction2(ctx, domex_ctor, "DOMException", 0, JS_CFUNC_constructor, 0);
    if (JS_IsException(g) || domex_def_consts(ctx, g) < 0) {
        JS_FreeValue(ctx, g);
        JS_FreeValue(ctx, proto);
        return -1;
    }
    JS_SetConstructor(ctx, g, proto);
    JS_FreeValue(ctx, g);
    JS_SetClassProto(ctx, domex_cid, proto);
    return 0;
}

static JSValue domex_global_get(JSContext* ctx, JSValueConst this_val)
{
    JSValue proto, ctor, g;
    (void)this_val;
    if (domex_ensure(ctx) < 0)
        return JS_EXCEPTION;
    proto = JS_GetClassProto(ctx, domex_cid);
    ctor = JS_GetPropertyStr(ctx, proto, "constructor");
    JS_FreeValue(ctx, proto);
    if (JS_IsException(ctor))
        return ctor;
    g = JS_GetGlobalObject(ctx);
    JS_DefinePropertyValueStr(ctx, g, "DOMException", JS_DupValue(ctx, ctor), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, g);
    return ctor;
}

static JSValue domex_global_set(JSContext* ctx, JSValueConst this_val, JSValueConst v)
{
    JSValue g = JS_GetGlobalObject(ctx);
    (void)this_val;
    JS_DefinePropertyValueStr(ctx, g, "DOMException", JS_DupValue(ctx, v), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, g);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry domex_global_funcs[] = {
    JS_CGETSET_DEF("DOMException", domex_global_get, domex_global_set),
};

void js_domex_install(JSContext* ctx, JSValueConst global_obj)
{
    JS_SetPropertyFunctionList(ctx, global_obj, domex_global_funcs, countof(domex_global_funcs));
}

JSValue js_dom_exception(JSContext* ctx, const char* name, const char* msg)
{
    if (domex_ensure(ctx) < 0)
        return JS_EXCEPTION;
    return domex_make(ctx, JS_UNDEFINED, JS_NewString(ctx, msg ? msg : ""), JS_NewString(ctx, name));
}

JSValue js_throw_dom_exception(JSContext* ctx, const char* name, const char* fmt, ...)
{
    va_list ap;
    JSValue e, m;
    va_start(ap, fmt);
    m = JS_NewStringVF(ctx, fmt, ap);
    va_end(ap);
    if (JS_IsException(m))
        return m;
    if (domex_ensure(ctx) < 0) {
        JS_FreeValue(ctx, m);
        return JS_EXCEPTION;
    }
    e = domex_make(ctx, JS_UNDEFINED, m, JS_NewString(ctx, name));
    if (JS_IsException(e))
        return e;
    return JS_Throw(ctx, e);
}

int js_is_dom_exception(JSValueConst v)
{
    return domex_cid && JS_GetOpaque(v, domex_cid) != NULL;
}

JSValue js_dom_exception_copy(JSContext* ctx, JSValueConst v)
{
    DomEx* d = JS_GetOpaque(v, domex_cid);
    if (!d)
        return JS_ThrowTypeError(ctx, "not a DOMException");
    return domex_make(ctx, JS_UNDEFINED, JS_DupValue(ctx, d->message), JS_DupValue(ctx, d->name));
}
