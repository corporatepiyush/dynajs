#include "dynajs.h"
#include <math.h>

#define countof(x) (sizeof(x) / sizeof((x)[0]))

typedef struct {
    int x;
    int y;
} JSPointData;

static JSClassID js_point_class_id;

static void js_point_finalizer(JSRuntime* rt, JSValue val)
{
    JSPointData* s = JS_GetOpaque(val, js_point_class_id);
    js_free_rt(rt, s);
}

static JSValue js_point_ctor(JSContext* ctx,
    JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    JSPointData* s;
    JSValue obj = JS_UNDEFINED;
    JSValue proto;

    s = js_mallocz(ctx, sizeof(*s));
    if (!s)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &s->x, argv[0]))
        goto fail;
    if (JS_ToInt32(ctx, &s->y, argv[1]))
        goto fail;
    proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto))
        goto fail;
    obj = JS_NewObjectProtoClass(ctx, proto, js_point_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        goto fail;
    JS_SetOpaque(obj, s);
    return obj;
fail:
    js_free(ctx, s);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_point_get_xy(JSContext* ctx, JSValueConst this_val, int magic)
{
    JSPointData* s = JS_GetOpaque2(ctx, this_val, js_point_class_id);
    if (!s)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewInt32(ctx, s->x);
    else
        return JS_NewInt32(ctx, s->y);
}

static JSValue js_point_set_xy(JSContext* ctx, JSValueConst this_val, JSValue val, int magic)
{
    JSPointData* s = JS_GetOpaque2(ctx, this_val, js_point_class_id);
    int v;
    if (!s)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &v, val))
        return JS_EXCEPTION;
    if (magic == 0)
        s->x = v;
    else
        s->y = v;
    return JS_UNDEFINED;
}

static JSValue js_point_norm(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSPointData* s = JS_GetOpaque2(ctx, this_val, js_point_class_id);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, sqrt((double)s->x * s->x + (double)s->y * s->y));
}

static JSClassDef js_point_class = {
    "Point",
    .finalizer = js_point_finalizer,
};

static const JSCFunctionListEntry js_point_proto_funcs[] = {
    JS_CGETSET_MAGIC_DEF("x", js_point_get_xy, js_point_set_xy, 0),
    JS_CGETSET_MAGIC_DEF("y", js_point_get_xy, js_point_set_xy, 1),
    JS_CFUNC_DEF("norm", 0, js_point_norm),
};

static int js_point_init(JSContext* ctx, JSModuleDef* m)
{
    JSValue point_proto, point_class;

    JS_NewClassID(&js_point_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_point_class_id, &js_point_class);

    point_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, point_proto, js_point_proto_funcs, countof(js_point_proto_funcs));

    point_class = JS_NewCFunction2(ctx, js_point_ctor, "Point", 2, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, point_class, point_proto);
    JS_SetClassProto(ctx, js_point_class_id, point_proto);

    JS_SetModuleExport(ctx, m, "Point", point_class);
    return 0;
}

JSModuleDef* js_init_module(JSContext* ctx, const char* module_name);

JSModuleDef* js_init_module(JSContext* ctx, const char* module_name)
{
    JSModuleDef* m;
    m = JS_NewCModule(ctx, module_name, js_point_init);
    if (!m)
        return NULL;
    JS_AddModuleExport(ctx, m, "Point");
    return m;
}
