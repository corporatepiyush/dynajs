#include "file-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_FILE)

void dyn_fh_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_fh_t* f = (dyn_fh_t*)JS_GetOpaque(val, dyn_fh_class_id);
    if (!f)
        return;
    JS_FreeValueRT(rt, f->path);
    free(f);
}

void dyn_fh_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    dyn_fh_t* f = (dyn_fh_t*)JS_GetOpaque(val, dyn_fh_class_id);
    if (f)
        JS_MarkValue(rt, f->path, mark_func);
}

JSValue dyn_fh_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_fh_t* f;
    JSValue obj, path, proto;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "new File(path) requires a Path");
    proto = dyn_ctor_proto(ctx, new_target, dyn_fh_class_id);
    if (JS_IsException(proto))
        return proto;
    if (dyn_is_path(argv[0])) {
        path = JS_DupValue(ctx, argv[0]);
    } else if (JS_IsString(argv[0])) {
        path = dyn_path_ctor(ctx, JS_UNDEFINED, 1, argv);
        if (JS_IsException(path)) {
            JS_FreeValue(ctx, proto);
            return path;
        }
    } else {
        JS_FreeValue(ctx, proto);
        return JS_ThrowTypeError(ctx, "new File(path): path must be a Path or a string");
    }

    f = (dyn_fh_t*)calloc(1, sizeof(*f));
    if (!f) {
        JS_FreeValue(ctx, proto);
        JS_FreeValue(ctx, path);
        return JS_ThrowOutOfMemory(ctx);
    }
    f->path = path;
    obj = JS_NewObjectProtoClass(ctx, proto, dyn_fh_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, path);
        free(f);
        return obj;
    }
    JS_SetOpaque(obj, f);
    return obj;
}

dyn_fh_t* dyn_fh_of(JSContext* ctx, JSValueConst v)
{
    return (dyn_fh_t*)JS_GetOpaque2(ctx, v, dyn_fh_class_id);
}

JSValue dyn_fh_append(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_fh_t* f = dyn_fh_of(ctx, this_val);
    JSValueConst a[3];
    JSValue opts, r;

    if (!f)
        return JS_EXCEPTION;
    opts = JS_NewObject(ctx);
    if (JS_IsException(opts))
        return opts;
    JS_DefinePropertyValueStr(ctx, opts, "append", JS_NewBool(ctx, 1), JS_PROP_C_W_E);
    a[0] = f->path;
    a[1] = (argc > 0) ? argv[0] : JS_UNDEFINED;
    a[2] = opts;
    r = dyn_file_write_file(ctx, JS_UNDEFINED, 3, a);
    JS_FreeValue(ctx, opts);
    return r;
}

JSValue dyn_fh_move_to(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_fh_t* f = dyn_fh_of(ctx, this_val);
    JSValueConst a[2];
    JSValue r;

    if (!f)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "moveTo(dest) requires a Path");
    a[0] = f->path;
    a[1] = argv[0];
    r = dyn_fs_rename(ctx, JS_UNDEFINED, 2, a);
    if (JS_IsException(r))
        return r;
    JS_FreeValue(ctx, r);
    r = JS_IsString(argv[0]) ? dyn_path_ctor(ctx, JS_UNDEFINED, 1, argv) : JS_DupValue(ctx, argv[0]);
    if (JS_IsException(r))
        return r;
    JS_FreeValue(ctx, f->path);
    f->path = r;
    return JS_DupValue(ctx, this_val);
}

JSValue dyn_fh_copy_to(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_fh_t* f = dyn_fh_of(ctx, this_val);
    JSValueConst a[3];

    if (!f)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "copyTo(dest[, options]) requires a Path");
    a[0] = f->path;
    a[1] = argv[0];
    a[2] = (argc > 1) ? argv[1] : JS_UNDEFINED;
    {
        JSValue r = dyn_fs_copy_file(ctx, JS_UNDEFINED, 3, a);
        if (JS_IsException(r))
            return r;
        JS_FreeValue(ctx, r);
    }
    JSValueConst one[1];
    one[0] = argv[0];
    return dyn_fh_ctor(ctx, JS_UNDEFINED, 1, one);
}

JSValue dyn_fh_stream(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    dyn_fh_t* f = dyn_fh_of(ctx, this_val);
    JSValueConst a[2];

    if (!f)
        return JS_EXCEPTION;
    a[0] = f->path;
    a[1] = (argc > 0) ? argv[0] : JS_UNDEFINED;
    return magic ? dyn_fwriter_ctor(ctx, JS_UNDEFINED, 2, a)
                 : dyn_freader_ctor(ctx, JS_UNDEFINED, 2, a);
}

JSValue dyn_fh_get_path(JSContext* ctx, JSValueConst this_val)
{
    dyn_fh_t* f = dyn_fh_of(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, f->path);
}

JSValue dyn_fh_to_string(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_fh_t* f = dyn_fh_of(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!f)
        return JS_EXCEPTION;
    return dyn_path_get_str(ctx, f->path);
}

#endif
