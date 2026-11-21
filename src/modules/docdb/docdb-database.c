#include "docdb-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DOCDB)

static void db_dispose(void* p)
{
    DocDb* d = p;
    if (!d)
        return;
    docdb_streams_detach(d);
    dd_db_free(d->db);
    free(d);
}

static int ring_cap_opt(JSContext* ctx, JSValueConst opts, uint32_t* out)
{
    uint64_t v = 0;
    if (docdb_opt_u64(ctx, opts, "changeStreamCapacity", &v) < 0)
        return -1;
    if (v == 0)
        return 0;
    if (v > (1u << 24)) {
        docdb_throw_code(ctx, DD_E_BADVALUE, "changeStreamCapacity must be at most %u", 1u << 24);
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static JSValue db_new_obj(JSContext* ctx, JSValueConst new_target, const char* name, size_t n, JSValueConst opts)
{
    DocDb* d;
    char* nm;
    uint32_t cap = 0;
    if (ring_cap_opt(ctx, opts, &cap) < 0)
        return JS_EXCEPTION;
    d = calloc(1, sizeof *d);
    nm = malloc(n + 1);
    if (!d || !nm) {
        free(d);
        free(nm);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(nm, name, n);
    nm[n] = 0;
    d->db = dd_db_new(nm);
    free(nm);
    if (!d->db) {
        free(d);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (cap)
        d->db->ring_default = (int)cap;
    d->ctx = ctx;
    d->db->owner = d;
    d->db->on_drop = docdb_streams_on_drop;
    return dyn_res_wrap(ctx, new_target, docdb_db_class_id, d, db_dispose);
}

JSValue docdb_db_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    const char* name = "test";
    size_t n = 4;
    JSValue r;
    int own = 0;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (!JS_IsString(argv[0]))
            return JS_ThrowTypeError(ctx, "Database(name): name must be a string");
        name = JS_ToCStringLen(ctx, &n, argv[0]);
        if (!name)
            return JS_EXCEPTION;
        own = 1;
        if (n == 0 || memchr(name, 0, n) || memchr(name, '.', n) || memchr(name, '/', n)) {
            JS_FreeCString(ctx, name);
            return docdb_throw_code(ctx, DD_E_BADVALUE, "invalid database name");
        }
    }
    r = db_new_obj(ctx, new_target, name, n, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (own)
        JS_FreeCString(ctx, name);
    return r;
}

static const char* path_arg(JSContext* ctx, int argc, JSValueConst* argv, const char* who)
{
    if (argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx, "%s(path): path must be a string", who);
        return NULL;
    }
    return JS_ToCString(ctx, argv[0]);
}

JSValue docdb_db_load_static(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* path = path_arg(ctx, argc, argv, "Database.load");
    JSValue obj, p;
    DocDb* d;
    (void)this_val;
    if (!path)
        return JS_EXCEPTION;
    obj = db_new_obj(ctx, JS_UNDEFINED, "test", 4, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (JS_IsException(obj)) {
        JS_FreeCString(ctx, path);
        return obj;
    }
    d = docdb_db_get(ctx, obj);
    p = d ? docdb_load(ctx, obj, d, path, 3) : JS_EXCEPTION;
    JS_FreeCString(ctx, path);
    JS_FreeValue(ctx, obj);
    return p;
}

static JSValue db_name(JSContext* ctx, JSValueConst this_val)
{
    DocDb* d = docdb_db_get(ctx, this_val);
    if (!d)
        return JS_EXCEPTION;
    return JS_NewString(ctx, d->db->name ? d->db->name : "");
}

static JSValue name_arg(JSContext* ctx, int argc, JSValueConst* argv, const char** s, size_t* n, const char* who)
{
    dd_err e = { 0 };
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "%s(name): name must be a string", who);
    *s = JS_ToCStringLen(ctx, n, argv[0]);
    if (!*s)
        return JS_EXCEPTION;
    if (dd_coll_name_ok(*s, *n, &e) < 0) {
        JS_FreeCString(ctx, *s);
        *s = NULL;
        return docdb_throw(ctx, &e);
    }
    return JS_UNDEFINED;
}

static JSValue db_collection(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    DocDb* d = docdb_db_get(ctx, this_val);
    const char* s = NULL;
    size_t n;
    JSValue r;
    if (!d)
        return JS_EXCEPTION;
    if (JS_IsException(name_arg(ctx, argc, argv, &s, &n, "collection")))
        return JS_EXCEPTION;
    r = docdb_coll_new(ctx, this_val, s, n);
    JS_FreeCString(ctx, s);
    return r;
}

static JSValue db_create_collection(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    DocDb* d = docdb_db_get(ctx, this_val);
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    const char* s = NULL;
    size_t n;
    dd_coll* c;
    dd_err e = { 0 };
    uint32_t cap = 0;
    JSValue r;
    dyn_bson_w ob;
    if (!d)
        return JS_EXCEPTION;
    if (JS_IsException(name_arg(ctx, argc, argv, &s, &n, "createCollection")))
        return JS_EXCEPTION;
    if (dd_db_find(d->db, s, n)) {
        r = docdb_throw_code(ctx, DD_E_NSEXISTS, "Collection %s.%s already exists.", d->db->name ? d->db->name : "", s);
        JS_FreeCString(ctx, s);
        return r;
    }
    dyn_bson_w_init(&ob);
    if (ring_cap_opt(ctx, opts, &cap) < 0 || docdb_encode_opt_doc(ctx, opts, &ob, "options") < 0) {
        JS_FreeCString(ctx, s);
        dyn_bson_w_free(&ob);
        return JS_EXCEPTION;
    }
    c = dd_db_create(d->db, s, n, &e);
    if (!c) {
        JS_FreeCString(ctx, s);
        dyn_bson_w_free(&ob);
        return docdb_throw(ctx, &e);
    }
    d->gen++;
    if (cap)
        c->ring_cap = cap;
    if (ob.n > 5) {
        c->options = ob.p;
        c->options_n = ob.n;
        ob.p = NULL;
    }
    dyn_bson_w_free(&ob);
    r = docdb_coll_new(ctx, this_val, s, n);
    JS_FreeCString(ctx, s);
    return r;
}

static JSValue db_list_collections(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    DocDb* d = docdb_db_get(ctx, this_val);
    JSValue arr;
    int i;
    (void)argc;
    (void)argv;
    if (!d)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    for (i = 0; i < d->db->ncoll && !JS_IsException(arr); i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewStringLen(ctx, d->db->coll[i]->name, d->db->coll[i]->name_n));
    return arr;
}

static JSValue db_drop_collection(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    DocDb* d = docdb_db_get(ctx, this_val);
    const char* s = NULL;
    size_t n;
    int r;
    if (!d)
        return JS_EXCEPTION;
    if (JS_IsException(name_arg(ctx, argc, argv, &s, &n, "dropCollection")))
        return JS_EXCEPTION;
    r = dd_db_drop(d->db, s, n);
    JS_FreeCString(ctx, s);
    d->gen++;
    return JS_NewBool(ctx, r);
}

static JSValue db_persist(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    DocDb* d = docdb_db_get(ctx, this_val);
    const char* path;
    int do_fsync;
    JSValue r;
    if (!d)
        return JS_EXCEPTION;
    if (docdb_opt_bool(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "fsync", 1, &do_fsync) < 0)
        return JS_EXCEPTION;
    path = path_arg(ctx, argc, argv, "persist");
    if (!path)
        return JS_EXCEPTION;
    r = docdb_persist(ctx, this_val, d, NULL, path, do_fsync);
    JS_FreeCString(ctx, path);
    return r;
}

static JSValue db_load(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    DocDb* d = docdb_db_get(ctx, this_val);
    const char* path;
    int merge;
    JSValue r;
    if (!d)
        return JS_EXCEPTION;
    if (docdb_opt_bool(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "merge", 0, &merge) < 0)
        return JS_EXCEPTION;
    path = path_arg(ctx, argc, argv, "load");
    if (!path)
        return JS_EXCEPTION;
    r = docdb_load(ctx, this_val, d, path, merge ? 0 : 1);
    JS_FreeCString(ctx, path);
    return r;
}

static JSValue db_stats(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    DocDb* d = docdb_db_get(ctx, this_val);
    JSValue r;
    double docs = 0, bytes = 0, ixs = 0, keys = 0;
    int i, k;
    (void)argc;
    (void)argv;
    if (!d)
        return JS_EXCEPTION;
    for (i = 0; i < d->db->ncoll; i++) {
        dd_coll* c = d->db->coll[i];
        docs += c->count;
        bytes += (double)c->arena.bytes;
        ixs += c->nix;
        for (k = 0; k < c->nix; k++)
            keys += (double)c->ix[k]->entries;
    }
    r = JS_NewObject(ctx);
    if (JS_IsException(r))
        return r;
    JS_DefinePropertyValueStr(ctx, r, "collections", JS_NewInt32(ctx, d->db->ncoll), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, r, "documents", JS_NewFloat64(ctx, docs), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, r, "dataBytes", JS_NewFloat64(ctx, bytes), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, r, "indexes", JS_NewFloat64(ctx, ixs), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, r, "indexEntries", JS_NewFloat64(ctx, keys), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, r, "retiredBytes", JS_NewFloat64(ctx, (double)d->db->retired_bytes), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, r, "pinnedSnapshots", JS_NewInt32(ctx, d->db->npins), JS_PROP_C_W_E);
    return r;
}

const JSCFunctionListEntry docdb_db_funcs[] = {
    JS_CGETSET_DEF("name", db_name, NULL),
    JS_CFUNC_DEF("collection", 1, db_collection),
    JS_CFUNC_DEF("createCollection", 1, db_create_collection),
    JS_CFUNC_DEF("listCollections", 0, db_list_collections),
    JS_CFUNC_DEF("dropCollection", 1, db_drop_collection),
    JS_CFUNC_DEF("persist", 1, db_persist),
    JS_CFUNC_DEF("load", 1, db_load),
    JS_CFUNC_DEF("stats", 0, db_stats),
    JS_CFUNC_MAGIC_DEF("watch", 0, docdb_watch, 1),
};

const int docdb_db_nfuncs = (int)countof(docdb_db_funcs);

#endif
