#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include "docdb-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DOCDB)

JSClassID docdb_db_class_id;
JSClassID docdb_coll_class_id;
JSClassID docdb_err_class_id;

int64_t docdb_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static JSValue docdb_make_error(JSContext* ctx, int code, const char* msg)
{
    JSValue err = JS_NewError(ctx), proto;
    if (JS_IsException(err))
        return err;
    proto = JS_GetClassProto(ctx, docdb_err_class_id);
    if (JS_IsObject(proto))
        JS_SetPrototype(ctx, err, proto);
    JS_FreeValue(ctx, proto);
    JS_DefinePropertyValueStr(ctx, err, "message", JS_NewString(ctx, msg), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, err, "code", JS_NewInt32(ctx, code), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, err, "codeName", JS_NewString(ctx, dd_code_name(code)), JS_PROP_C_W_E);
    return err;
}

JSValue docdb_throw(JSContext* ctx, const dd_err* e)
{
    JSValue err;
    if (JS_HasException(ctx))
        return JS_EXCEPTION;
    if (e->code == DD_E_OOM)
        return JS_ThrowOutOfMemory(ctx);
    err = docdb_make_error(ctx, e->code ? e->code : DD_E_INTERNAL, e->msg[0] ? e->msg : "docdb error");
    if (JS_IsException(err))
        return err;
    return JS_Throw(ctx, err);
}

JSValue docdb_throw_code(JSContext* ctx, int code, const char* fmt, ...)
{
    dd_err e;
    va_list ap;
    e.code = code;
    va_start(ap, fmt);
    vsnprintf(e.msg, sizeof e.msg, fmt, ap);
    va_end(ap);
    return docdb_throw(ctx, &e);
}

static JSValue docdb_err_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    const char* msg = argc > 0 && !JS_IsUndefined(argv[0]) ? JS_ToCString(ctx, argv[0]) : NULL;
    int32_t code = DD_E_INTERNAL;
    JSValue err;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && !msg)
        return JS_EXCEPTION;
    if (argc > 1 && JS_ToInt32(ctx, &code, argv[1])) {
        JS_FreeCString(ctx, msg);
        return JS_EXCEPTION;
    }
    err = docdb_make_error(ctx, code, msg ? msg : "");
    JS_FreeCString(ctx, msg);
    if (!JS_IsException(err) && JS_IsObject(new_target)) {
        JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
        if (JS_IsObject(proto))
            JS_SetPrototype(ctx, err, proto);
        JS_FreeValue(ctx, proto);
    }
    return err;
}

static int docdb_register_error(JSContext* ctx, JSModuleDef* m)
{
    JSClassDef def = { "DocDBError" };
    JSValue global, errctor, errproto, proto, ctor;
    JS_NewClassID(&docdb_err_class_id);
    if (!JS_IsRegisteredClass(JS_GetRuntime(ctx), docdb_err_class_id)
        && JS_NewClass(JS_GetRuntime(ctx), docdb_err_class_id, &def) < 0)
        return -1;
    global = JS_GetGlobalObject(ctx);
    errctor = JS_GetPropertyStr(ctx, global, "Error");
    JS_FreeValue(ctx, global);
    errproto = JS_GetPropertyStr(ctx, errctor, "prototype");
    JS_FreeValue(ctx, errctor);
    if (!JS_IsObject(errproto)) {
        JS_FreeValue(ctx, errproto);
        return -1;
    }
    proto = JS_NewObjectProto(ctx, errproto);
    JS_FreeValue(ctx, errproto);
    if (JS_IsException(proto))
        return -1;
    JS_DefinePropertyValueStr(ctx, proto, "name", JS_NewString(ctx, "DocDBError"), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, proto, "code", JS_NewInt32(ctx, DD_E_INTERNAL), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, proto, "codeName", JS_NewString(ctx, dd_code_name(DD_E_INTERNAL)), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, proto, "writeErrors", JS_UNDEFINED, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, proto, "result", JS_UNDEFINED, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    ctor = JS_NewCFunction2(ctx, (JSCFunction*)docdb_err_ctor, "DocDBError", 2, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetClassProto(ctx, docdb_err_class_id, proto);
    return JS_SetModuleExport(ctx, m, "DocDBError", ctor);
}

static void docdb_coll_finalizer(JSRuntime* rt, JSValue val)
{
    DocColl* c = JS_GetOpaque(val, docdb_coll_class_id);
    if (!c)
        return;
    JS_FreeValueRT(rt, c->dbobj);
    free(c->name);
    free(c);
}

static void docdb_coll_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    DocColl* c = JS_GetOpaque(val, docdb_coll_class_id);
    if (c)
        JS_MarkValue(rt, c->dbobj, mark);
}

static JSValue docdb_coll_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    (void)new_target;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "Collection: use db.collection(name)");
}

DocDb* docdb_db_get(JSContext* ctx, JSValueConst v)
{
    return dyn_res_native(ctx, v, docdb_db_class_id);
}

JSValue docdb_coll_new(JSContext* ctx, JSValueConst dbobj, const char* name, size_t n)
{
    DocColl* c = calloc(1, sizeof *c);
    if (!c || !(c->name = malloc(n + 1))) {
        free(c);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(c->name, name, n);
    c->name[n] = 0;
    c->name_n = n;
    c->dbobj = JS_DupValue(ctx, dbobj);
    return dyn_plain_wrap(ctx, JS_UNDEFINED, docdb_coll_class_id, c, NULL);
}

dd_coll* docdb_coll_get(JSContext* ctx, JSValueConst this_val, DocDb** dbp, int create)
{
    DocColl* c = JS_GetOpaque2(ctx, this_val, docdb_coll_class_id);
    DocDb* d;
    dd_coll* dc;
    if (!c)
        return NULL;
    d = docdb_db_get(ctx, c->dbobj);
    if (!d)
        return NULL;
    if (dbp)
        *dbp = d;
    if (c->cached && c->cached_gen == d->gen)
        return c->cached;
    dc = dd_db_find(d->db, c->name, c->name_n);
    if (!dc && create > 0) {
        dd_err e = { 0 };
        dc = dd_db_create(d->db, c->name, c->name_n, &e);
        if (!dc) {
            docdb_throw(ctx, &e);
            return NULL;
        }
        d->gen++;
    }
    if (!dc) {
        if (create >= 0)
            docdb_throw_code(ctx, DD_E_NAMESPACENOTFOUND, "ns not found: %s", c->name);
        return NULL;
    }
    c->cached = dc;
    c->cached_gen = d->gen;
    return dc;
}

dd_coll* docdb_coll_read(JSContext* ctx, JSValueConst this_val, DocDb** dbp, dd_coll** tmp)
{
    DocDb* d = NULL;
    DocColl* dc;
    dd_coll* c = docdb_coll_get(ctx, this_val, &d, -1);
    *tmp = NULL;
    if (dbp)
        *dbp = d;
    if (c || JS_HasException(ctx) || !d)
        return c;
    dc = JS_GetOpaque(this_val, docdb_coll_class_id);
    *tmp = dd_coll_new(d->db, dc->name, dc->name_n);
    if (!*tmp)
        JS_ThrowOutOfMemory(ctx);
    return *tmp;
}

static const JSCFunctionListEntry docdb_db_static[] = {
    JS_CFUNC_DEF("load", 1, docdb_db_load_static),
};

static int docdb_init_module(JSContext* ctx, JSModuleDef* m)
{
    static JSClassDef db_def = { "Database", .finalizer = dyn_res_finalizer };
    static JSClassDef coll_def = { "Collection", .finalizer = docdb_coll_finalizer, .gc_mark = docdb_coll_mark };
    const DynBsonJsKinds* k;
    JSValue ctor, proto;
    if (dyn_register_class(ctx, m, &docdb_db_class_id, &db_def, docdb_db_funcs, docdb_db_nfuncs,
            (JSCFunction*)docdb_db_ctor, "Database")
        < 0)
        return -1;
    proto = JS_GetClassProto(ctx, docdb_db_class_id);
    ctor = JS_GetPropertyStr(ctx, proto, "constructor");
    JS_FreeValue(ctx, proto);
    JS_SetPropertyFunctionList(ctx, ctor, docdb_db_static, countof(docdb_db_static));
    JS_FreeValue(ctx, ctor);
    if (dyn_register_plain_class(ctx, m, &docdb_coll_class_id, &coll_def, docdb_coll_funcs, docdb_coll_nfuncs,
            (JSCFunction*)docdb_coll_ctor, "Collection")
        < 0)
        return -1;
    if (docdb_register_error(ctx, m) < 0)
        return -1;
    if (docdb_register_stream(ctx, m) < 0)
        return -1;
    if (dyn_bsonjs_ensure(ctx, &k) < 0)
        return -1;
    return JS_SetModuleExport(ctx, m, "ObjectId", dyn_bsonjs_ctor(ctx, DYN_BSONJS_OBJECTID));
}

int js_nat_init_docdb(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:docdb", docdb_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Database");
    JS_AddModuleExport(ctx, m, "Collection");
    JS_AddModuleExport(ctx, m, "DocDBError");
    JS_AddModuleExport(ctx, m, "ObjectId");
    JS_AddModuleExport(ctx, m, "ChangeStream");
    return 0;
}

#endif
