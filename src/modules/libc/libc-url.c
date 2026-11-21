#include "libc-url.h"

#define DYN_URL_MAX 65536

JSClassID dyn_url_class_id;

static void dyn_url_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_url_t* u = (dyn_url_t*)JS_GetOpaque(val, dyn_url_class_id);
    if (!u)
        return;
    if (--u->refs == 0) {
        dyn_url_free(u);
        return;
    }
    JS_FreeValueRT(rt, dyn_url_obj(u)->sp_wrapper);
}

static void dyn_url_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    dyn_url_t* u = (dyn_url_t*)JS_GetOpaque(val, dyn_url_class_id);
    if (u)
        JS_MarkValue(rt, dyn_url_obj(u)->sp_wrapper, mark);
}

static JSClassDef dyn_url_class = { "URL", .finalizer = dyn_url_finalizer,
    .gc_mark = dyn_url_gc_mark };

JSValue dyn_url_href(JSContext* ctx, const dyn_url_t* u)
{
    dyn_sb_t b;
    JSValue r;

    dyn_sb_init(&b);
    dyn_url_serialize(&b, u);
    if (b.oom) {
        dyn_sb_free(&b);
        return JS_ThrowOutOfMemory(ctx);
    }
    r = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
    dyn_sb_free(&b);
    return r;
}

JSValue dyn_url_build(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv, int quiet)
{
    const char *s = NULL, *bs = NULL;
    const char *s_js = NULL, *bs_js = NULL;
    char *cs = NULL, *cbs = NULL;
    size_t n = 0, bn = 0;
    dyn_url_t *u = NULL, base;
    int have_base = 0;

    memset(&base, 0, sizeof base);
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "new URL(input[, base]): input is required");
    s_js = JS_ToCStringLenUTF8(ctx, &n, argv[0]);
    if (!s_js)
        return JS_EXCEPTION;
    s = s_js;
    cs = dyn_strip_tabnl(s, n, &n);
    if (cs)
        s = cs;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        bs_js = JS_ToCStringLenUTF8(ctx, &bn, argv[1]);
        if (!bs_js)
            goto fail;
        bs = bs_js;
        cbs = dyn_strip_tabnl(bs, bn, &bn);
        if (cbs)
            bs = cbs;
        if (bn > DYN_URL_MAX || dyn_url_parse(&base, bs, bn, NULL) < 0) {
            if (quiet)
                goto quiet_fail;
            JS_ThrowTypeError(ctx, "new URL(input, base): base is not a valid URL");
            goto fail;
        }
        have_base = 1;
    }
    if (n > DYN_URL_MAX) {
        if (quiet)
            goto quiet_fail;
        JS_ThrowRangeError(ctx, "new URL(input): input exceeds %u bytes", DYN_URL_MAX);
        goto fail;
    }
    u = (dyn_url_t*)calloc(1, sizeof(dyn_urlobj_t));
    if (!u) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    dyn_url_obj(u)->sp_wrapper = JS_UNDEFINED;
    if (dyn_url_parse(u, s, n, have_base ? &base : NULL) < 0) {
        dyn_url_free(u);
        u = NULL;
        if (quiet)
            goto quiet_fail;
        JS_ThrowTypeError(ctx, "new URL(input): \"%.*s\" is not a valid URL",
            (int)(n > 80 ? 80 : n), s);
        goto fail;
    }
    free(cs);
    free(cbs);
    JS_FreeCString(ctx, s_js);
    if (bs_js)
        JS_FreeCString(ctx, bs_js);
    free(base.scheme);
    free(base.username);
    free(base.password);
    free(base.host);
    free(base.port);
    free(base.path);
    free(base.query);
    free(base.fragment);
    free(base.drive);
    u->refs = 1;
    {
        JSValue proto, obj;
        if (JS_IsUndefined(new_target)) {
            proto = JS_GetClassProto(ctx, dyn_url_class_id);
        } else {
            proto = JS_GetPropertyStr(ctx, new_target, "prototype");
            if (JS_IsException(proto)) {
                u->refs = 0;
                dyn_url_free(u);
                return proto;
            }
        }
        if (!JS_IsObject(proto)) {
            JS_FreeValue(ctx, proto);
            proto = JS_GetClassProto(ctx, dyn_url_class_id);
        }
        obj = JS_NewObjectProtoClass(ctx, proto, dyn_url_class_id);
        JS_FreeValue(ctx, proto);
        if (JS_IsException(obj)) {
            u->refs = 0;
            dyn_url_free(u);
            return obj;
        }
        JS_SetOpaque(obj, u);
        return obj;
    }
quiet_fail:
    free(cs);
    free(cbs);
    if (s_js)
        JS_FreeCString(ctx, s_js);
    if (bs_js)
        JS_FreeCString(ctx, bs_js);
    free(base.scheme);
    free(base.username);
    free(base.password);
    free(base.host);
    free(base.port);
    free(base.path);
    free(base.query);
    free(base.fragment);
    free(base.drive);
    return JS_NULL;
fail:
    free(cs);
    free(cbs);
    if (s_js)
        JS_FreeCString(ctx, s_js);
    if (bs_js)
        JS_FreeCString(ctx, bs_js);
    free(base.scheme);
    free(base.username);
    free(base.password);
    free(base.host);
    free(base.port);
    free(base.path);
    free(base.query);
    free(base.fragment);
    free(base.drive);
    return JS_EXCEPTION;
}

JSValue js_url_parse_static(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    return dyn_url_build(ctx, JS_UNDEFINED, argc, argv, 1);
}

static JSValue dyn_url_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    return dyn_url_build(ctx, new_target, argc, argv, 0);
}

static JSValue js_url_canparse_static(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue r;
    (void)this_val;
    r = dyn_url_build(ctx, JS_UNDEFINED, argc, argv, 1);
    if (JS_IsException(r))
        return r;
    JS_FreeValue(ctx, r);
    return JS_IsNull(r) ? JS_FALSE : JS_TRUE;
}

static JSValue js_url_join_static(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue args[2], obj, r;
    dyn_url_t* u;
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "URL.join(base, rel): base and rel are required");
    args[0] = argv[1];
    args[1] = argv[0];
    obj = dyn_url_build(ctx, JS_UNDEFINED, 2, args, 0);
    if (JS_IsException(obj))
        return obj;
    u = dyn_url_of(ctx, obj);
    if (!u) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    r = dyn_url_href(ctx, u);
    JS_FreeValue(ctx, obj);
    return r;
}

static JSValue dyn_url_tostring(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_url_t* u = dyn_url_of(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!u)
        return JS_EXCEPTION;
    return dyn_url_href(ctx, u);
}

static const JSCFunctionListEntry dyn_url_proto[] = {
    JS_CGETSET_MAGIC_DEF("href", dyn_url_get, NULL, U_HREF),
    JS_CGETSET_MAGIC_DEF("protocol", dyn_url_get, dyn_url_set, U_PROTOCOL),
    JS_CGETSET_MAGIC_DEF("username", dyn_url_get, dyn_url_set, U_USERNAME),
    JS_CGETSET_MAGIC_DEF("password", dyn_url_get, dyn_url_set, U_PASSWORD),
    JS_CGETSET_MAGIC_DEF("host", dyn_url_get, dyn_url_set, U_HOST),
    JS_CGETSET_MAGIC_DEF("hostname", dyn_url_get, dyn_url_set, U_HOSTNAME),
    JS_CGETSET_MAGIC_DEF("port", dyn_url_get, dyn_url_set, U_PORT),
    JS_CGETSET_MAGIC_DEF("pathname", dyn_url_get, dyn_url_set, U_PATHNAME),
    JS_CGETSET_MAGIC_DEF("search", dyn_url_get, dyn_url_set, U_SEARCH),
    JS_CGETSET_MAGIC_DEF("hash", dyn_url_get, dyn_url_set, U_HASH),
    JS_CGETSET_MAGIC_DEF("origin", dyn_url_get, NULL, U_ORIGIN),
    JS_CGETSET_MAGIC_DEF("searchParams", dyn_url_get, dyn_url_set, U_SEARCHPARAMS),
    JS_CFUNC_DEF("toString", 0, dyn_url_tostring),
    JS_CFUNC_DEF("toJSON", 0, dyn_url_tostring),
};

static JSClassID dyn_url_holder_id;
static const JSClassDef dyn_url_holder_class = { "URLHolder" };

static int dyn_url_class_ensure(JSRuntime* rt, JSClassID* pid, const JSClassDef* def)
{
    JS_NewClassID(pid);
    if (JS_IsRegisteredClass(rt, *pid))
        return 0;
    return JS_NewClass(rt, *pid, def);
}

JSValue dyn_url_constructor(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue holder, proto, ctor;
    if (dyn_url_class_ensure(rt, &dyn_url_class_id, &dyn_url_class) < 0
        || dyn_url_class_ensure(rt, &dyn_url_holder_id, &dyn_url_holder_class) < 0)
        return JS_ThrowInternalError(ctx, "URL: class registration failed");
    holder = JS_GetClassProto(ctx, dyn_url_holder_id);
    if (JS_IsObject(holder)) {
        ctor = JS_GetPropertyStr(ctx, holder, "ctor");
        JS_FreeValue(ctx, holder);
        return ctor;
    }
    JS_FreeValue(ctx, holder);
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return proto;
    JS_SetPropertyFunctionList(ctx, proto, dyn_url_proto, countof(dyn_url_proto));
    JS_SetClassProto(ctx, dyn_url_class_id, JS_DupValue(ctx, proto));
    ctor = JS_NewCFunction2(ctx, dyn_url_ctor, "URL", 0, JS_CFUNC_constructor, 0);
    if (JS_IsException(ctor)) {
        JS_FreeValue(ctx, proto);
        return ctor;
    }
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    if (JS_DefinePropertyValueStr(ctx, ctor, "parse",
            JS_NewCFunction(ctx, js_url_parse_static, "parse", 2), JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, ctor, "canParse",
               JS_NewCFunction(ctx, js_url_canparse_static, "canParse", 2), JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, ctor, "join",
               JS_NewCFunction(ctx, js_url_join_static, "join", 2), JS_PROP_C_W_E)
            < 0) {
        JS_FreeValue(ctx, ctor);
        return JS_EXCEPTION;
    }
    holder = JS_NewObjectProto(ctx, JS_NULL);
    if (JS_IsException(holder)) {
        JS_FreeValue(ctx, ctor);
        return holder;
    }
    JS_SetPropertyStr(ctx, holder, "ctor", JS_DupValue(ctx, ctor));
    JS_SetClassProto(ctx, dyn_url_holder_id, holder);
    return ctor;
}

void dyn_url_install(JSContext* ctx, JSValueConst global_obj)
{
    JSValue ctor = dyn_url_constructor(ctx);
    if (JS_IsException(ctor)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    JS_DefinePropertyValueStr(ctx, global_obj, "URL", ctor, JS_PROP_C_W_E);
}
