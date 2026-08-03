/*
 * dynascript native modules -- shared framework (in-repo, no external deps).
 * See dyna-nat.h for the resource/ownership model.
 */
#include "dyna-nat.h"

#ifdef CONFIG_NATIVE_MODULES

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* The shared disk/network I/O primitives (dyn_iobuf_t and the dyn_io_ / dyn_net_
 * functions) now live in dyna-io.c (engine-core, always linked); dyna-nat.h
 * includes dyna-io.h so every module keeps seeing them. */

/* Registry of framework-owned class ids, so the shared close()/closed methods
 * can validate `this` before treating its opaque as a DynResource. A foreign
 * object passed via close.call(x) must never be reinterpreted (it may store a
 * non-pointer opaque). Registration happens only from js_nat_init_all() on the
 * main context at startup -- worker threads never register modules -- so this
 * table is not concurrently mutated. Class ids are process-unique and never
 * reset; the cap is sized well beyond the number of native classes. */
#define DYN_MAX_CLASSES 256
static JSClassID dyn_class_ids[DYN_MAX_CLASSES];
static int dyn_n_classes;

static int dyn_is_our_class(JSClassID id)
{
    int i;
    for (i = 0; i < dyn_n_classes; i++)
        if (dyn_class_ids[i] == id)
            return 1;
    return 0;
}

/* Idempotent teardown: run the module dispose exactly once. */
static void dyn_res_release(JSRuntime *rt, DynResource *r)
{
    (void)rt;
    if (!r || r->closed)
        return;
    r->closed = 1;
    if (r->dispose && r->native)
        r->dispose(r->native);
    r->native = NULL;
}

void dyn_res_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id;
    DynResource *r = JS_GetAnyOpaque(val, &id);
    /* only reached for our classes (set in their JSClassDef), so `r` is ours */
    if (r) {
        dyn_res_release(rt, r);
        js_free_rt(rt, r);
    }
}

JSValue dyn_res_wrap(JSContext *ctx, JSClassID class_id, void *native,
                     DynDisposeFunc dispose)
{
    JSValue obj;
    DynResource *r;

    obj = JS_NewObjectClass(ctx, class_id);
    if (JS_IsException(obj))
        goto fail;
    r = js_mallocz(ctx, sizeof(*r));
    if (!r) {
        JS_FreeValue(ctx, obj);
        goto fail;
    }
    r->native = native;
    r->dispose = dispose;
    r->closed = 0;
    JS_SetOpaque(obj, r);
    return obj;
 fail:
    if (dispose && native)
        dispose(native);
    return JS_EXCEPTION;
}

DynResource *dyn_res_get(JSContext *ctx, JSValueConst this_val,
                         JSClassID class_id)
{
    DynResource *r = JS_GetOpaque2(ctx, this_val, class_id);
    if (!r)
        return NULL; /* JS_GetOpaque2 already threw */
    if (r->closed) {
        JS_ThrowTypeError(ctx, "use of a closed native resource");
        return NULL;
    }
    return r;
}

/* close() / dispose(): explicit deterministic release. Safe for any `this`. */
static JSValue dyn_method_close(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSClassID id;
    DynResource *r = JS_GetAnyOpaque(this_val, &id);
    (void)argc; (void)argv;
    if (r && dyn_is_our_class(id))
        dyn_res_release(JS_GetRuntime(ctx), r);
    return JS_UNDEFINED;
}

static JSValue dyn_getter_closed(JSContext *ctx, JSValueConst this_val)
{
    JSClassID id;
    DynResource *r = JS_GetAnyOpaque(this_val, &id);
    if (r && dyn_is_our_class(id))
        return JS_NewBool(ctx, r->closed);
    return JS_ThrowTypeError(ctx, "not a native resource");
}

/* close/dispose/[Symbol.dispose]/closed installed on every resource proto.
 * [Symbol.dispose] makes these work with `using` and DisposableStack.use(). */
static const JSCFunctionListEntry dyn_common_funcs[] = {
    JS_CFUNC_DEF("close", 0, dyn_method_close),
    JS_CFUNC_DEF("dispose", 0, dyn_method_close),
    JS_CFUNC_DEF("[Symbol.dispose]", 0, dyn_method_close),
    JS_CGETSET_DEF("closed", dyn_getter_closed, NULL),
};

void dyn_res_class_common(JSContext *ctx, JSClassID class_id, JSValue proto)
{
    if (dyn_n_classes < DYN_MAX_CLASSES)
        dyn_class_ids[dyn_n_classes++] = class_id;
    JS_SetPropertyFunctionList(ctx, proto, dyn_common_funcs,
                               countof(dyn_common_funcs));
}

/* ---- Plain GC-managed classes (no close/dispose surface) ---- */

JSValue dyn_plain_wrap(JSContext *ctx, JSClassID class_id, void *native,
                       DynDisposeFunc dispose)
{
    JSValue obj = JS_NewObjectClass(ctx, class_id);
    if (JS_IsException(obj)) {
        if (dispose && native)
            dispose(native);
        return obj;
    }
    JS_SetOpaque(obj, native);
    return obj;
}

int dyn_register_plain_class(JSContext *ctx, JSModuleDef *m, JSClassID *pid,
                             const JSClassDef *def,
                             const JSCFunctionListEntry *proto_funcs,
                             int n_funcs, JSCFunction *ctor_fn,
                             const char *name)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue proto, ctor;

    JS_NewClassID(pid);
    if (JS_NewClass(rt, *pid, def) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, proto_funcs, n_funcs);
    /* NB: no dyn_res_class_common -- a plain class carries no close surface. */
    JS_SetClassProto(ctx, *pid, proto);
    ctor = JS_NewCFunction2(ctx, ctor_fn, name, 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    return JS_SetModuleExport(ctx, m, name, ctor);
}

int dyn_register_class(JSContext *ctx, JSModuleDef *m, JSClassID *pid,
                       const JSClassDef *def,
                       const JSCFunctionListEntry *proto_funcs, int n_funcs,
                       JSCFunction *ctor_fn, const char *name)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue proto, ctor;

    JS_NewClassID(pid);
    if (JS_NewClass(rt, *pid, def) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, proto_funcs, n_funcs);
    dyn_res_class_common(ctx, *pid, proto);
    JS_SetClassProto(ctx, *pid, proto);
    ctor = JS_NewCFunction2(ctx, ctor_fn, name, 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    return JS_SetModuleExport(ctx, m, name, ctor);
}

/* Path lives in dyna-file.c. When that family is not compiled there is no Path
 * class at all, so every other module's path argument is unsatisfiable -- and
 * saying so is better than failing to link. */
#ifndef CONFIG_NATIVE_MODULE_FILE
const char *dyn_path_borrow(JSContext *ctx, JSValueConst v, const char *what,
                            size_t *plen)
{
    (void)v; (void)plen;
    JS_ThrowTypeError(ctx, "%s must be a Path, but dyna:file is not built in",
                      what);
    return NULL;
}
int dyn_value_is_path(JSValueConst v) { (void)v; return 0; }
#endif

int js_nat_init_all(JSContext *ctx)
{
#ifdef CONFIG_NATIVE_MODULE_STRUCTURES
    if (js_nat_init_structures(ctx))
        return -1;
    if (js_nat_init_structures_ext(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_NET
    if (js_nat_init_net(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_ML
    if (js_nat_init_ml(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_COMPRESS
    if (js_nat_init_compress(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_RANDOM
    if (js_nat_init_random(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_STRUCTURES3
    if (js_nat_init_structures3(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_SIMD
    if (js_nat_init_simd(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_FILE
    if (js_nat_init_file(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_SEMVER
    if (js_nat_init_semver(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_BYTES
    if (js_nat_init_bytes(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_CRYPTO
    if (js_nat_init_hash(ctx))
        return -1;
    if (js_nat_init_crypto(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_MATCHER
    if (js_nat_init_matcher(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_ENCODING
    if (js_nat_init_encoding(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_TIME
    if (js_nat_init_time(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_MATHX
    if (js_nat_init_mathx(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_CSV
    if (js_nat_init_csv(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_DATAFRAME
    if (js_nat_init_dataframe(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_UUID
    if (js_nat_init_uuid(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_CONFIG
    if (js_nat_init_config(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_LOG
    if (js_nat_init_log(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_URL
    if (js_nat_init_url(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_TERM
    if (js_nat_init_term(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_VALIDATE
    if (js_nat_init_validate(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_XML
    if (js_nat_init_xml(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_YAML
    if (js_nat_init_yaml(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_DECIMAL
    if (js_nat_init_decimal(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_VSERIALIZE
    if (js_nat_init_vserialize(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_HTML
    if (js_nat_init_html(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_SYS
    if (js_nat_init_sys(ctx))
        return -1;
#endif
#ifdef CONFIG_NATIVE_MODULE_SCRAPE
    if (js_nat_init_scrape(ctx))
        return -1;
#endif
#if defined(CONFIG_IO_URING) && defined(__linux__)
    if (js_nat_init_uring(ctx))
        return -1;
#endif
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES */
