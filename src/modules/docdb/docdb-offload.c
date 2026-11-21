#include "docdb-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DOCDB)

typedef struct {
    JSValue dbobj;
    DocDb* d;
    DynResource* res;
    char* path;
    int do_fsync;
    uint64_t epoch;
    dd_snap_coll* colls;
    const uint8_t*** docbufs;
    dyn_bson_w* metas;
    int ncoll;
    dyn_bson_w catalog;
    uint64_t bytes;
    uint64_t docs;
    dd_err err;
    int rc;
    int fresh;
    int resolve_db;
    dd_db* scratch;
    dd_snap_loaded loaded;
} DocJob;

static void job_free(JSContext* ctx, DocJob* j)
{
    int i;
    for (i = 0; i < j->ncoll; i++) {
        free(j->colls[i].name);
        free(j->docbufs[i]);
        dyn_bson_w_free(&j->metas[i]);
    }
    free(j->colls);
    free(j->docbufs);
    free(j->metas);
    dyn_bson_w_free(&j->catalog);
    dd_snap_loaded_free(&j->loaded);
    if (j->scratch)
        dd_db_free(j->scratch);
    free(j->path);
    if (j->res && j->res->async_users > 0)
        j->res->async_users--;
    if (j->d)
        j->d->async_jobs--;
    JS_FreeValue(ctx, j->dbobj);
    free(j);
}

static DocJob* job_new(JSContext* ctx, JSValueConst dbobj, DocDb* d, const char* path)
{
    DocJob* j = calloc(1, sizeof *j);
    if (!j || !(j->path = strdup(path))) {
        free(j);
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    j->dbobj = JS_DupValue(ctx, dbobj);
    j->d = d;
    j->res = dyn_res_get(ctx, dbobj, docdb_db_class_id);
    if (j->res)
        j->res->async_users++;
    d->async_jobs++;
    dyn_bson_w_init(&j->catalog);
    return j;
}

static JSValue persist_entry(JSContext* ctx, void* arg)
{
    DocJob* j = arg;
    JSValue r;
    dyn_fiber_offload();
    j->rc = dd_snap_write(j->path, j->catalog.p, j->colls, j->ncoll, j->do_fsync, &j->bytes, &j->err);
    dyn_fiber_onjs();
    dd_db_unpin(j->d->db, j->epoch);
    if (j->rc < 0) {
        docdb_throw(ctx, &j->err);
        job_free(ctx, j);
        return JS_EXCEPTION;
    }
    r = JS_NewObject(ctx);
    if (!JS_IsException(r)) {
        JS_DefinePropertyValueStr(ctx, r, "path", JS_NewString(ctx, j->path), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, r, "bytes", JS_NewFloat64(ctx, (double)j->bytes), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, r, "collections", JS_NewInt32(ctx, j->ncoll), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, r, "documents", JS_NewFloat64(ctx, (double)j->docs), JS_PROP_C_W_E);
    }
    job_free(ctx, j);
    return r;
}

static int snap_coll(DocJob* j, dd_coll* c)
{
    dd_snap_coll* sc = &j->colls[j->ncoll];
    dyn_bson_w* m = &j->metas[j->ncoll];
    const uint8_t** docs;
    dyn_bson_w_init(m);
    sc->name = malloc(c->name_n + 1);
    docs = malloc((size_t)(c->nslots ? c->nslots : 1) * sizeof *docs);
    if (!sc->name || !docs || dd_snap_coll_meta(c, m) < 0) {
        free(sc->name);
        sc->name = NULL;
        free(docs);
        dyn_bson_w_free(m);
        return -1;
    }
    j->docbufs[j->ncoll] = docs;
    memcpy(sc->name, c->name, c->name_n + 1);
    sc->name_n = c->name_n;
    if (c->nslots)
        memcpy(docs, c->doc, (size_t)c->nslots * sizeof *docs);
    sc->docs = docs;
    sc->ndocs = c->nslots;
    sc->meta = m->p;
    j->docs += c->count;
    j->ncoll++;
    return 0;
}

JSValue docdb_persist(JSContext* ctx, JSValueConst dbobj, DocDb* d, dd_coll* only, const char* path, int do_fsync)
{
    DocJob* j = job_new(ctx, dbobj, d, path);
    int i, n = only ? 1 : d->db->ncoll;
    JSValue p;
    if (!j)
        return JS_EXCEPTION;
    j->do_fsync = do_fsync;
    j->colls = calloc((size_t)(n ? n : 1), sizeof *j->colls);
    j->metas = calloc((size_t)(n ? n : 1), sizeof *j->metas);
    j->docbufs = calloc((size_t)(n ? n : 1), sizeof *j->docbufs);
    if (!j->colls || !j->metas || !j->docbufs)
        goto oom;
    dyn_bson_w_doc_begin(&j->catalog);
    if (d->db->name)
        dyn_bson_w_string(&j->catalog, "name", 4, d->db->name, strlen(d->db->name));
    dyn_bson_w_doc_end(&j->catalog);
    if (j->catalog.oom)
        goto oom;
    for (i = 0; i < n; i++)
        if (snap_coll(j, only ? only : d->db->coll[i]) < 0)
            goto oom;
    j->epoch = dd_db_pin(d->db);
    if (!j->epoch)
        goto oom;
    p = dyn_fiber_start(ctx, persist_entry, j);
    if (JS_IsException(p)) {
        dd_db_unpin(d->db, j->epoch);
        job_free(ctx, j);
    }
    return p;
oom:
    job_free(ctx, j);
    return JS_ThrowOutOfMemory(ctx);
}

static JSValue load_finish(JSContext* ctx, DocJob* j)
{
    DocDb* d = j->d;
    JSValue r;
    int i;
    if (j->fresh)
        while (d->db->ncoll)
            dd_db_drop(d->db, d->db->coll[0]->name, d->db->coll[0]->name_n);
    for (i = 0; i < j->loaded.ncoll; i++) {
        dd_coll* c = j->loaded.colls[i];
        if (dd_db_find(d->db, c->name, c->name_n))
            dd_db_drop(d->db, c->name, c->name_n);
        c->db = d->db;
        c->arena.db = d->db;
        if (dd_db_install(d->db, c) < 0) {
            while (i < j->loaded.ncoll)
                dd_coll_free(j->loaded.colls[i++]);
            j->loaded.ncoll = 0;
            d->gen++;
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    d->gen++;
    if (j->resolve_db) {
        dyn_bson_val nm, user;
        if (dyn_bson_find(j->loaded.catalog, "user", 4, &user) && user.type == DYN_BSON_DOC
            && dyn_bson_find(user.val, "name", 4, &nm) && nm.type == DYN_BSON_STRING) {
            char* s = strdup((const char*)nm.val + 4);
            if (s) {
                free(d->db->name);
                d->db->name = s;
            }
        }
        r = JS_DupValue(ctx, j->dbobj);
    } else {
        r = JS_NewObject(ctx);
        if (!JS_IsException(r)) {
            JS_DefinePropertyValueStr(ctx, r, "path", JS_NewString(ctx, j->path), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, r, "bytes", JS_NewFloat64(ctx, (double)j->loaded.bytes), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, r, "collections", JS_NewInt32(ctx, j->loaded.ncoll), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, r, "documents", JS_NewFloat64(ctx, (double)j->loaded.docs), JS_PROP_C_W_E);
        }
    }
    free(j->loaded.colls);
    j->loaded.colls = NULL;
    j->loaded.ncoll = 0;
    return r;
}

static JSValue load_entry(JSContext* ctx, void* arg)
{
    DocJob* j = arg;
    JSValue r;
    dyn_fiber_offload();
    j->rc = dd_snap_read(j->path, j->scratch, &j->loaded, &j->err);
    dyn_fiber_onjs();
    if (j->rc < 0) {
        docdb_throw(ctx, &j->err);
        job_free(ctx, j);
        return JS_EXCEPTION;
    }
    r = load_finish(ctx, j);
    job_free(ctx, j);
    return r;
}

JSValue docdb_load(JSContext* ctx, JSValueConst dbobj, DocDb* d, const char* path, int fresh)
{
    DocJob* j = job_new(ctx, dbobj, d, path);
    JSValue p;
    if (!j)
        return JS_EXCEPTION;
    j->fresh = fresh & 1;
    j->resolve_db = (fresh & 2) != 0;
    j->scratch = dd_db_new("load");
    if (!j->scratch) {
        job_free(ctx, j);
        return JS_ThrowOutOfMemory(ctx);
    }
    p = dyn_fiber_start(ctx, load_entry, j);
    if (JS_IsException(p))
        job_free(ctx, j);
    return p;
}

#endif
