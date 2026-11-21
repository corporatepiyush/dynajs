#include "sqlite-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SQLITE) && defined(CONFIG_SQLITE)

#define SQL_CACHE_DEFAULT 32

JSClassID dyn_sqlite_class_id;

static const JSClassDef dyn_sqlite_class = {
    "SQLite",
    .finalizer = dyn_res_finalizer,
};

static void dyn_sqlite_dispose(void* native)
{
    dyn_sqlite_t* s = (dyn_sqlite_t*)native;
    if (!s)
        return;
    sql_cache_clear(s);
    if (s->db)
        sqlite3_close_v2(s->db);
    free(s);
}

int sqlite_progress(void* ud)
{
    dyn_sqlite_t* s = (dyn_sqlite_t*)ud;
    if (s->interrupted)
        return 1;
    if (JS_CheckInterrupt(s->ctx)) {
        s->interrupted = 1;
        return 1;
    }
    return 0;
}

static int sqlite_ro_authorizer(void* ud, int action, const char* a1,
    const char* a2, const char* db,
    const char* trigger)
{
    (void)ud;
    (void)a1;
    (void)a2;
    (void)db;
    (void)trigger;
    return action == SQLITE_ATTACH ? SQLITE_DENY : SQLITE_OK;
}

static void sqlite_default_wal(sqlite3* db)
{
    sqlite3_stmt* st = NULL;
    int wal = 0;

    if (sqlite3_prepare_v2(db, "PRAGMA journal_mode=WAL", -1, &st, NULL) != SQLITE_OK)
        return;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* mode = sqlite3_column_text(st, 0);
        wal = mode && strcmp((const char*)mode, "wal") == 0;
    }
    sqlite3_finalize(st);
    if (wal)
        sqlite3_exec(db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
}

static JSValue dyn_sqlite_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_sqlite_t* s;
    const char* path = NULL;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, ro = 0, big = 0, wal = 1;
    JSValue v;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "new SQLite(path, options?)");
    {
        size_t pn = 0;
        path = JS_ToCStringLen(ctx, &pn, argv[0]);
        if (!path)
            return JS_EXCEPTION;
        if (memchr(path, '\0', pn)) {
            JS_FreeCString(ctx, path);
            return JS_ThrowTypeError(ctx,
                "SQLite: path must not contain a NUL byte");
        }
    }
    if (argc > 1 && JS_IsObject(argv[1])) {
        v = JS_GetPropertyStr(ctx, argv[1], "readonly");
        ro = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "bigint");
        big = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "wal");
        if (!JS_IsUndefined(v))
            wal = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
    }
    if (ro)
        flags = SQLITE_OPEN_READONLY;

    s = (dyn_sqlite_t*)calloc(1, sizeof(*s));
    if (!s) {
        JS_FreeCString(ctx, path);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->ctx = ctx;
    s->bigint = big;
    s->cache_max = SQL_CACHE_DEFAULT;
    if (sqlite3_open_v2(path, &s->db, flags | SQLITE_OPEN_NOMUTEX,
            NULL)
        != SQLITE_OK) {
        JSValue e = JS_ThrowInternalError(ctx, "SQLite: cannot open '%s': %s",
            path,
            s->db ? sqlite3_errmsg(s->db)
                  : "out of memory");
        JS_FreeCString(ctx, path);
        if (s->db)
            sqlite3_close_v2(s->db);
        free(s);
        return e;
    }
    if (ro)
        sqlite3_set_authorizer(s->db, sqlite_ro_authorizer, NULL);
    else if (wal)
        sqlite_default_wal(s->db);
    sqlite3_progress_handler(s->db, SQL_PROGRESS_OPS, sqlite_progress, s);
#ifdef SQLITE_DBCONFIG_DEFENSIVE
    sqlite3_db_config(s->db, SQLITE_DBCONFIG_DEFENSIVE, 1, (int*)NULL);
#endif
#ifdef SQLITE_DBCONFIG_TRUSTED_SCHEMA
    sqlite3_db_config(s->db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, (int*)NULL);
#endif
    if (dyn_nat_limit())
        sqlite3_hard_heap_limit64((sqlite3_int64)dyn_nat_limit());
    JS_FreeCString(ctx, path);
    return dyn_res_wrap(ctx, new_target, dyn_sqlite_class_id, s, dyn_sqlite_dispose);
}

static JSValue dyn_sqlite_query(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    return sqlite_run(ctx, this_val, argc, argv, 1);
}

static JSValue dyn_sqlite_exec(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    return sqlite_run(ctx, this_val, argc, argv, 0);
}

static JSValue dyn_sqlite_exec_many(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return sqlite_run_many(ctx, this_val, argc, argv);
}

static JSValue dyn_sqlite_last_id(JSContext* ctx, JSValueConst this_val)
{
    dyn_sqlite_t* s = (dyn_sqlite_t*)dyn_res_native(ctx, this_val,
        dyn_sqlite_class_id);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, sqlite3_last_insert_rowid(s->db));
}

static JSValue dyn_sqlite_version(JSContext* ctx, JSValueConst this_val)
{
    (void)this_val;
    return JS_NewString(ctx, sqlite3_libversion());
}

static const JSCFunctionListEntry dyn_sqlite_proto[] = {
    JS_CFUNC_DEF("query", 1, dyn_sqlite_query),
    JS_CFUNC_DEF("exec", 1, dyn_sqlite_exec),
    JS_CFUNC_DEF("execMany", 2, dyn_sqlite_exec_many),
    JS_CGETSET_DEF("lastInsertRowId", dyn_sqlite_last_id, NULL),
    JS_CGETSET_DEF("version", dyn_sqlite_version, NULL),
};

static int dyn_sqlite_register(JSContext* ctx, JSModuleDef* m)
{
    return dyn_register_class(ctx, m, &dyn_sqlite_class_id, &dyn_sqlite_class,
        dyn_sqlite_proto, countof(dyn_sqlite_proto),
        dyn_sqlite_ctor, "SQLite");
}

static void dyn_sqlite_add_exports(JSContext* ctx, JSModuleDef* m)
{
    JS_AddModuleExport(ctx, m, "SQLite");
}


int js_nat_init_sqlite(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:sqlite", dyn_sqlite_register);
    if (!m)
        return -1;
    dyn_sqlite_add_exports(ctx, m);
    return 0;
}

#endif
