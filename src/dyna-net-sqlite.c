#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET) && defined(CONFIG_SQLITE)

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

typedef struct {
    char* sql;
    size_t sqllen;
    uint32_t hash;
    sqlite3_stmt* st;
    uint64_t lastuse;
} sql_cache_ent_t;

typedef struct {
    JSContext* ctx;
    sqlite3* db;
    int bigint;
    sql_cache_ent_t* ents;
    int nents, cap_ents;
    int cache_max;
    int interrupted;
    uint64_t use_clock;
} dyn_sqlite_t;

static JSClassID dyn_sqlite_class_id;

#define SQL_CACHE_DEFAULT 32

static uint32_t sql_hash(const char* s, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++)
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static sql_cache_ent_t* sql_cache_find(dyn_sqlite_t* s, const char* sql,
    size_t n, uint32_t h)
{
    int i;
    for (i = 0; i < s->nents; i++)
        if (s->ents[i].hash == h && s->ents[i].sqllen == n && memcmp(s->ents[i].sql, sql, n) == 0) {
            s->ents[i].lastuse = ++s->use_clock;
            return &s->ents[i];
        }
    return NULL;
}

static int sql_cache_put(dyn_sqlite_t* s, const char* sql, size_t n,
    uint32_t h, sqlite3_stmt* st)
{
    sql_cache_ent_t* e;

    if (s->cache_max <= 0 || n > (size_t)s->cache_max * 4096)
        return -1;
    if (s->nents >= s->cache_max) {
        int i, victim = 0;
        for (i = 1; i < s->nents; i++)
            if (s->ents[i].lastuse < s->ents[victim].lastuse)
                victim = i;
        free(s->ents[victim].sql);
        sqlite3_finalize(s->ents[victim].st);
        s->ents[victim] = s->ents[--s->nents];
    } else if (s->nents == s->cap_ents) {
        int cap = s->cap_ents ? s->cap_ents * 2 : 8;
        sql_cache_ent_t* ne = (sql_cache_ent_t*)realloc(s->ents,
            (size_t)cap * sizeof(*ne));
        if (!ne)
            return -1;
        s->ents = ne;
        s->cap_ents = cap;
    }
    e = &s->ents[s->nents++];
    e->sql = (char*)malloc(n + 1);
    if (!e->sql) {
        s->nents--;
        return -1;
    }
    memcpy(e->sql, sql, n);
    e->sql[n] = '\0';
    e->sqllen = n;
    e->hash = h;
    e->st = st;
    e->lastuse = ++s->use_clock;
    return 0;
}

static void sql_cache_drop_stmt(dyn_sqlite_t* s, sqlite3_stmt* st)
{
    int i;
    for (i = 0; i < s->nents; i++)
        if (s->ents[i].st == st) {
            free(s->ents[i].sql);
            s->ents[i] = s->ents[--s->nents];
            return;
        }
}

static void sql_cache_clear(dyn_sqlite_t* s)
{
    int i;
    for (i = 0; i < s->nents; i++) {
        free(s->ents[i].sql);
        sqlite3_finalize(s->ents[i].st);
    }
    free(s->ents);
    s->ents = NULL;
    s->nents = s->cap_ents = 0;
}
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

#define SQL_PROGRESS_OPS 10000

static int sqlite_progress(void* ud)
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

static JSValue sqlite_throw(JSContext* ctx, dyn_sqlite_t* s, const char* what)
{
    if (s->interrupted) {
        s->interrupted = 0;
        return JS_EXCEPTION;
    }
    return JS_ThrowInternalError(ctx, "SQLite: %s: %s", what,
        s->db ? sqlite3_errmsg(s->db) : "no database");
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

static JSValue dyn_sqlite_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_sqlite_t* s;
    const char* path = NULL;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, ro = 0, big = 0;
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

static JSValue sqlite_bytes(JSContext* ctx, const uint8_t* p, size_t n)
{
    JSValue ab = JS_NewArrayBufferCopy(ctx, p, n), ta;
    JSValueConst a3[3];
    if (JS_IsException(ab))
        return ab;
    a3[0] = ab;
    a3[1] = JS_NewInt32(ctx, 0);
    a3[2] = JS_NewInt32(ctx, (int)n);
    ta = JS_NewTypedArray(ctx, 3, a3, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return ta;
}

static JSValue sqlite_column(JSContext* ctx, dyn_sqlite_t* s, sqlite3_stmt* st,
    int i)
{
    switch (sqlite3_column_type(st, i)) {
    case SQLITE_INTEGER: {
        sqlite3_int64 n = sqlite3_column_int64(st, i);
        if (n > 9007199254740992LL || n < -9007199254740992LL) {
            char buf[32];
            if (s->bigint)
                return JS_NewBigInt64(ctx, (int64_t)n);
            snprintf(buf, sizeof(buf), "%lld", (long long)n);
            return JS_NewString(ctx, buf);
        }
        return s->bigint ? JS_NewBigInt64(ctx, (int64_t)n)
                         : JS_NewInt64(ctx, n);
    }
    case SQLITE_FLOAT:
        return JS_NewFloat64(ctx, sqlite3_column_double(st, i));
    case SQLITE_NULL:
        return JS_NULL;
    case SQLITE_BLOB: {
        const void* b = sqlite3_column_blob(st, i);
        int n = sqlite3_column_bytes(st, i);
        return sqlite_bytes(ctx, (const uint8_t*)(b ? b : ""),
            (size_t)(n > 0 ? n : 0));
    }
    default: {
        const unsigned char* t = sqlite3_column_text(st, i);
        int n = sqlite3_column_bytes(st, i);
        return JS_NewStringLen(ctx, (const char*)(t ? t : (const unsigned char*)""),
            (size_t)(n > 0 ? n : 0));
    }
    }
}

static int sql_tail_empty(const char* p)
{
    while (*p) {
        if (*p == ';') {
            p++;
            continue;
        }
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
            continue;
        }
        return 0;
    }
    return 1;
}

typedef struct {
    uint8_t kind;
    int64_t i;
    double d;
    uint8_t* bytes;
    size_t blen;
} sqlite_cparam_t;

#define SQL_CP_NULL 0
#define SQL_CP_INT 1
#define SQL_CP_DOUBLE 2
#define SQL_CP_TEXT 3
#define SQL_CP_BLOB 4

static void sqlite_cparams_free(JSContext* ctx, sqlite_cparam_t* ps,
    uint32_t n)
{
    uint32_t i;
    (void)ctx;
    for (i = 0; i < n; i++)
        free(ps[i].bytes);
    free(ps);
}

static int sqlite_coerce_one(JSContext* ctx, JSValueConst v,
    sqlite_cparam_t* out, int idx)
{
    memset(out, 0, sizeof(*out));
    if (JS_IsNull(v) || JS_IsUndefined(v)) {
        out->kind = SQL_CP_NULL;
        return 0;
    }
    if (JS_IsBool(v)) {
        out->kind = SQL_CP_INT;
        out->i = JS_ToBool(ctx, v) ? 1 : 0;
        return 0;
    }
    if (JS_IsBigInt(ctx, v)) {
        int64_t n;
        if (JS_ToBigInt64(ctx, &n, v))
            return -1;
        out->kind = SQL_CP_INT;
        out->i = n;
        return 0;
    }
    if (JS_IsObject(v)) {
        size_t off = 0, len = 0, bpe = 0, total = 0;
        uint8_t* base;
        JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
        if (JS_IsException(ab)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            base = JS_GetArrayBuffer(ctx, &total, v);
            if (!base) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                JS_ThrowTypeError(ctx,
                    "SQLite: parameter %d is an object; a parameter is a value. "
                    "Pass a Uint8Array or ArrayBuffer for a BLOB, "
                    "JSON.stringify(v) for JSON, or an ISO string for a date",
                    idx);
                return -1;
            }
            off = 0;
            len = total;
        } else {
            base = JS_GetArrayBuffer(ctx, &total, ab);
            JS_FreeValue(ctx, ab);
            if (!base) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                return -1;
            }
        }
        if (off > total || len > total - off) {
            JS_ThrowRangeError(ctx,
                "SQLite: blob parameter view is out of bounds");
            return -1;
        }
        if (len > 0x7fffffff) {
            JS_ThrowRangeError(ctx, "SQLite: blob parameter exceeds 2GiB");
            return -1;
        }
        out->bytes = (uint8_t*)malloc(len ? len : 1);
        if (!out->bytes) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memcpy(out->bytes, base + off, len);
        out->kind = SQL_CP_BLOB;
        out->blen = len;
        return 0;
    }
    if (JS_IsNumber(v)) {
        double d;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        if (d >= -9223372036854775808.0 && d < 9223372036854775808.0) {
            int64_t asint = (int64_t)d;
            if (d == (double)asint) {
                out->kind = SQL_CP_INT;
                out->i = asint;
                return 0;
            }
        }
        out->kind = SQL_CP_DOUBLE;
        out->d = d;
        return 0;
    }
    {
        size_t len = 0;
        const char* str = JS_ToCStringLen(ctx, &len, v);
        if (!str)
            return -1;
        if (len > 0x7fffffff) {
            JS_FreeCString(ctx, str);
            JS_ThrowRangeError(ctx, "SQLite: text parameter exceeds 2GiB");
            return -1;
        }
        out->bytes = (uint8_t*)malloc(len ? len : 1);
        if (!out->bytes) {
            JS_FreeCString(ctx, str);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memcpy(out->bytes, str, len);
        JS_FreeCString(ctx, str);
        out->kind = SQL_CP_TEXT;
        out->blen = len;
        return 0;
    }
}

static int sqlite_bind_cparam(JSContext* ctx, sqlite3_stmt* st, int idx,
    const sqlite_cparam_t* p)
{
    int rc;
    switch (p->kind) {
    case SQL_CP_NULL:
        rc = sqlite3_bind_null(st, idx);
        break;
    case SQL_CP_INT:
        rc = sqlite3_bind_int64(st, idx, p->i);
        break;
    case SQL_CP_DOUBLE:
        rc = sqlite3_bind_double(st, idx, p->d);
        break;
    case SQL_CP_TEXT:
        rc = sqlite3_bind_text(st, idx, (const char*)p->bytes,
            (int)p->blen, SQLITE_TRANSIENT);
        break;
    case SQL_CP_BLOB:
        rc = sqlite3_bind_blob(st, idx, p->blen ? (const void*)p->bytes : "",
            (int)p->blen, SQLITE_TRANSIENT);
        break;
    default:
        rc = SQLITE_OK;
        break;
    }
    if (rc != SQLITE_OK) {
        JS_ThrowInternalError(ctx, "SQLite: cannot bind parameter %d", idx);
        return -1;
    }
    return 0;
}

static int64_t sql_run_rest(JSContext* ctx, dyn_sqlite_t* s, const char* tail)
{
    sqlite3_stmt* st = NULL;
    const char* p = tail;
    int64_t changes = 0;
    int rc;

    while (p && *p) {
        while (*p == ';')
            p++;
        if (!*p)
            break;
        if (sqlite3_prepare_v2(s->db, p, -1, &st, &p) != SQLITE_OK) {
            sqlite_throw(ctx, s, "prepare failed");
            return -1;
        }
        if (!st)
            continue;
        if (sqlite3_bind_parameter_count(st) > 0) {
            sqlite3_finalize(st);
            JS_ThrowRangeError(ctx,
                "SQLite: only the first statement of a multi-statement exec "
                "can take parameters");
            return -1;
        }
        {
            int base = sqlite3_total_changes(s->db);
            while ((rc = sqlite3_step(st)) == SQLITE_ROW)
                continue;
            if (rc != SQLITE_DONE) {
                sqlite_throw(ctx, s, "step failed");
                sqlite3_finalize(st);
                return -1;
            }
            if (sqlite3_total_changes(s->db) != base)
                changes += sqlite3_changes(s->db);
        }
        sqlite3_finalize(st);
    }
    return changes;
}

static JSValue sqlite_run(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int want_rows)
{
    dyn_sqlite_t* s;
    sqlite3_stmt* st = NULL;
    const char* sql;
    size_t sqllen;
    uint32_t hash;
    sql_cache_ent_t* ent;
    int cached = 0;
    char* key = NULL;
    const char* tail = NULL;
    JSValue rows = JS_UNDEFINED;
    uint32_t nrow = 0;
    int rc, i, nparam = 0, changes_base = 0;
    sqlite_cparam_t* cps = NULL;
    int64_t max_rows = 0;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "sql string required");
    sql = JS_ToCStringLen(ctx, &sqllen, argv[0]);
    if (!sql)
        return JS_EXCEPTION;
    if (strlen(sql) != sqllen) {
        JS_FreeCString(ctx, sql);
        return JS_ThrowTypeError(ctx,
            "SQLite: the SQL text contains U+0000; SQLite would stop reading there");
    }

    if (argc > 1 && JS_IsArray(ctx, argv[1])) {
        JSValue lenv = JS_GetPropertyStr(ctx, argv[1], "length");
        int64_t n = 0;
        if (JS_ToInt64(ctx, &n, lenv)) {
            JS_FreeValue(ctx, lenv);
            JS_FreeCString(ctx, sql);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lenv);
        nparam = (int)n;
        if (nparam > 0) {
            cps = (sqlite_cparam_t*)calloc((size_t)nparam, sizeof(*cps));
            if (!cps) {
                JS_FreeCString(ctx, sql);
                return JS_ThrowOutOfMemory(ctx);
            }
        }
        for (i = 0; i < nparam; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i);
            int bad = sqlite_coerce_one(ctx, v, &cps[i], i + 1);
            JS_FreeValue(ctx, v);
            if (bad) {
                JS_FreeCString(ctx, sql);
                sqlite_cparams_free(ctx, cps, (uint32_t)nparam);
                return JS_EXCEPTION;
            }
        }
    }

    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue mv = JS_GetPropertyStr(ctx, argv[2], "maxRows");
        if (JS_IsException(mv)) {
            JS_FreeValue(ctx, mv);
            JS_FreeCString(ctx, sql);
            sqlite_cparams_free(ctx, cps, (uint32_t)nparam);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(mv) && !JS_IsNull(mv)) {
            int64_t mr = 0;
            if (JS_ToInt64(ctx, &mr, mv) || mr < 0) {
                JS_FreeValue(ctx, mv);
                JS_FreeCString(ctx, sql);
                sqlite_cparams_free(ctx, cps, (uint32_t)nparam);
                return JS_ThrowRangeError(ctx, "SQLite: maxRows must be >= 0");
            }
            max_rows = mr;
        }
        JS_FreeValue(ctx, mv);
    }

    s = (dyn_sqlite_t*)dyn_res_native(ctx, this_val, dyn_sqlite_class_id);
    if (!s) {
        JS_FreeCString(ctx, sql);
        sqlite_cparams_free(ctx, cps, (uint32_t)nparam);
        return JS_EXCEPTION;
    }

    hash = sql_hash(sql, sqllen);
    ent = sql_cache_find(s, sql, sqllen, hash);
    if (ent) {
        st = ent->st;
        sqlite3_reset(st);
        cached = 1;
    } else {
        if (sqlite3_prepare_v2(s->db, sql, -1, &st, &tail) != SQLITE_OK) {
            JSValue e = sqlite_throw(ctx, s, "prepare failed");
            JS_FreeCString(ctx, sql);
            sqlite_cparams_free(ctx, cps, (uint32_t)nparam);
            return e;
        }
        if (!st) {
            JS_FreeCString(ctx, sql);
            sqlite_cparams_free(ctx, cps, (uint32_t)nparam);
            return want_rows ? JS_NewArray(ctx) : JS_NewInt32(ctx, 0);
        }
        if (want_rows && !sql_tail_empty(tail)) {
            sqlite3_finalize(st);
            JS_FreeCString(ctx, sql);
            sqlite_cparams_free(ctx, cps, (uint32_t)nparam);
            return JS_ThrowRangeError(ctx,
                "SQLite: query runs one statement; this text carries a "
                "second -- split them or use exec()");
        }
    }
    if (!cached) {
        key = (char*)malloc(sqllen + 1);
        if (!key) {
            JS_FreeCString(ctx, sql);
            sqlite_cparams_free(ctx, cps, (uint32_t)nparam);
            return JS_ThrowOutOfMemory(ctx);
        }
        memcpy(key, sql, sqllen + 1);
    }
    JS_FreeCString(ctx, sql);

    if (argc > 1 && JS_IsArray(ctx, argv[1])) {
        if (nparam != sqlite3_bind_parameter_count(st)) {
            int want = sqlite3_bind_parameter_count(st);
            if (cached)
                sqlite3_reset(st);
            else
                sqlite3_finalize(st);
            sqlite_cparams_free(ctx, cps, (uint32_t)nparam);
            free(key);
            return JS_ThrowRangeError(ctx,
                "SQLite: statement takes %d parameters, %d given", want, nparam);
        }
        for (i = 0; i < nparam; i++) {
            if (sqlite_bind_cparam(ctx, st, i + 1, &cps[i]) < 0) {
                if (cached)
                    sqlite3_reset(st);
                else
                    sqlite3_finalize(st);
                sqlite_cparams_free(ctx, cps, (uint32_t)nparam);
                free(key);
                return JS_EXCEPTION;
            }
        }
    } else if (sqlite3_bind_parameter_count(st) > 0) {
        int want = sqlite3_bind_parameter_count(st);
        if (cached)
            sqlite3_reset(st);
        else
            sqlite3_finalize(st);
        sqlite_cparams_free(ctx, cps, (uint32_t)nparam);
        free(key);
        return JS_ThrowRangeError(ctx,
            "SQLite: statement takes %d parameters, none given", want);
    }
    sqlite_cparams_free(ctx, cps, (uint32_t)nparam);
    cps = NULL;

    if (want_rows) {
        rows = JS_NewArray(ctx);
        if (JS_IsException(rows))
            goto fail;
    }
    changes_base = sqlite3_total_changes(s->db);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (!want_rows)
            continue;
        if (max_rows > 0 && nrow >= max_rows) {
            JS_ThrowRangeError(ctx,
                "SQLite: result exceeds maxRows (%lld); add a LIMIT or raise the cap",
                (long long)max_rows);
            goto fail;
        }
        {
            int ncol = sqlite3_column_count(st);
            JSValue o = JS_NewObject(ctx);
            if (JS_IsException(o))
                goto fail;
            for (i = 0; i < ncol; i++) {
                const char* cn = sqlite3_column_name(st, i);
                JSValue cv = sqlite_column(ctx, s, st, i);
                if (cn)
                    JS_DefinePropertyValueStr(ctx, o, cn, cv, JS_PROP_C_W_E);
                else
                    JS_FreeValue(ctx, cv);
            }
            JS_DefinePropertyValueUint32(ctx, rows, nrow++, o, JS_PROP_C_W_E);
        }
    }
    if (rc != SQLITE_DONE) {
        JSValue e = sqlite_throw(ctx, s, "step failed");
        if (cached) {
            sql_cache_drop_stmt(s, st);
            sqlite3_finalize(st);
        } else
            sqlite3_finalize(st);
        if (want_rows)
            JS_FreeValue(ctx, rows);
        free(key);
        return e;
    }

    if (cached) {
        sqlite3_reset(st);
    } else {
        if (!want_rows && tail && !sql_tail_empty(tail)) {
            int64_t first = sqlite3_total_changes(s->db) != changes_base
                ? sqlite3_changes(s->db)
                : 0;
            int64_t more;
            sqlite3_finalize(st);
            more = sql_run_rest(ctx, s, tail);
            free(key);
            if (more < 0)
                return JS_EXCEPTION;
            return JS_NewInt64(ctx, first + more);
        }
        if (tail && sql_tail_empty(tail)) {
            if (sql_cache_put(s, key, sqllen, hash, st) < 0)
                sqlite3_finalize(st);
        } else
            sqlite3_finalize(st);
    }
    free(key);
    if (want_rows)
        return rows;
    return JS_NewInt32(ctx, sqlite3_total_changes(s->db) != changes_base
            ? sqlite3_changes(s->db)
            : 0);

fail:
    if (cached)
        sqlite3_reset(st);
    else
        sqlite3_finalize(st);
    JS_FreeValue(ctx, rows);
    free(key);
    return JS_EXCEPTION;
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
    JS_CGETSET_DEF("lastInsertRowId", dyn_sqlite_last_id, NULL),
    JS_CGETSET_DEF("version", dyn_sqlite_version, NULL),
};

int dyn_sqlite_register(JSContext* ctx, JSModuleDef* m)
{
    return dyn_register_class(ctx, m, &dyn_sqlite_class_id, &dyn_sqlite_class,
        dyn_sqlite_proto, countof(dyn_sqlite_proto),
        dyn_sqlite_ctor, "SQLite");
}

void dyn_sqlite_add_exports(JSContext* ctx, JSModuleDef* m)
{
    JS_AddModuleExport(ctx, m, "SQLite");
}

#endif
