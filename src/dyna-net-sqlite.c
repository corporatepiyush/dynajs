/*
 * SQLite client, part of dyna:net.
 *
 * Not a network client at all -- SQLite is a disk library with no socket. It
 * lives here because it was asked for alongside the others; its risks are disk
 * risks, and its tests are disk tests.
 *
 * SQLITE IS LINKED, NOT VENDORED. The amalgamation is ~9 MB and the system copy
 * is a real, patched, security-tracked build. The version therefore VARIES by
 * host -- macOS ships 3.51 while Homebrew has 3.53 -- so `SQLite.version` is
 * reported at runtime rather than assumed at compile time.
 *
 * PARAMETERS ARE BOUND, NEVER INTERPOLATED. Every value goes through
 * sqlite3_bind_*, so a string containing a quote is a string and not syntax.
 * There is deliberately no "build the SQL for me" helper: the moment one exists,
 * somebody concatenates user input into it.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET) && \
    defined(CONFIG_SQLITE)

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

typedef struct {
    char *sql;              /* the exact query text, its own copy */
    size_t sqllen;
    uint32_t hash;
    sqlite3_stmt *st;       /* prepared once; reset between uses */
    uint64_t lastuse;       /* LRU clock */
} sql_cache_ent_t;

typedef struct {
    JSContext *ctx;
    sqlite3 *db;
    int bigint;             /* 64-bit integers as BigInt rather than text */
    /* Prepared-statement cache. MEASURED: sqlite3_prepare_v2 sampled at 79%
     * of db.query() -- parse and plan dominate a small statement. A hit skips
     * both: reset + rebind + step. Keyed by EXACT text, LRU-evicted. A cached
     * statement is reset whenever it is not mid-use, so an error or a JS
     * exception can never leak a half-stepped read transaction. */
    sql_cache_ent_t *ents;
    int nents, cap_ents;
    int cache_max;          /* entries; 0 disables */
    uint64_t use_clock;
} dyn_sqlite_t;

static JSClassID dyn_sqlite_class_id;

#define SQL_CACHE_DEFAULT 32

static uint32_t sql_hash(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static sql_cache_ent_t *sql_cache_find(dyn_sqlite_t *s, const char *sql,
                                       size_t n, uint32_t h)
{
    int i;
    for (i = 0; i < s->nents; i++)
        if (s->ents[i].hash == h && s->ents[i].sqllen == n &&
            memcmp(s->ents[i].sql, sql, n) == 0) {
            s->ents[i].lastuse = ++s->use_clock;
            return &s->ents[i];
        }
    return NULL;
}

/* Insert a freshly prepared statement, taking ownership of it. The cache is
 * a fixed-size working set: full means evict the LRU entry, not grow. */
static void sql_cache_put(dyn_sqlite_t *s, const char *sql, size_t n,
                          uint32_t h, sqlite3_stmt *st)
{
    sql_cache_ent_t *e;

    if (s->cache_max <= 0 || n > (size_t)s->cache_max * 4096)
        return;             /* disabled, or absurdly large key text */
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
        sql_cache_ent_t *ne = (sql_cache_ent_t *)realloc(s->ents,
                                (size_t)cap * sizeof(*ne));
        if (!ne)
            return;
        s->ents = ne;
        s->cap_ents = cap;
    }
    e = &s->ents[s->nents++];
    e->sql = (char *)malloc(n + 1);
    if (!e->sql) { s->nents--; return; }
    memcpy(e->sql, sql, n);
    e->sql[n] = '\0';
    e->sqllen = n;
    e->hash = h;
    e->st = st;
    e->lastuse = ++s->use_clock;
}

/* Drop ONE cached entry by statement pointer (used when an error leaves the
 * statement suspect), without finalizing it -- the caller owns the step. */
static void sql_cache_drop_stmt(dyn_sqlite_t *s, sqlite3_stmt *st)
{
    int i;
    for (i = 0; i < s->nents; i++)
        if (s->ents[i].st == st) {
            free(s->ents[i].sql);
            s->ents[i] = s->ents[--s->nents];
            return;
        }
}

static void sql_cache_clear(dyn_sqlite_t *s)
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
    "SQLite", .finalizer = dyn_res_finalizer,
};

static void dyn_sqlite_dispose(void *native)
{
    dyn_sqlite_t *s = (dyn_sqlite_t *)native;
    if (!s)
        return;
    sql_cache_clear(s);        /* finalize every cached statement first */
    if (s->db)
        sqlite3_close_v2(s->db);   /* _v2 tolerates unfinalised statements */
    free(s);
}

static JSValue sqlite_throw(JSContext *ctx, dyn_sqlite_t *s, const char *what)
{
    return JS_ThrowInternalError(ctx, "SQLite: %s: %s", what,
                                 s->db ? sqlite3_errmsg(s->db) : "no database");
}

/* A readonly handle must be unable to write ANYWHERE. ATTACH would open a
 * second database file -- writable, since the authorizer's notion of the
 * connection is per-main -- and every statement through it would bypass the
 * flag the caller was promised. Deny ATTACH outright; deny nothing else. */
static int sqlite_ro_authorizer(void *ud, int action, const char *a1,
                                const char *a2, const char *db,
                                const char *trigger)
{
    (void)ud; (void)a1; (void)a2; (void)db; (void)trigger;
    return action == SQLITE_ATTACH ? SQLITE_DENY : SQLITE_OK;
}

static JSValue dyn_sqlite_ctor(JSContext *ctx, JSValueConst new_target,
                               int argc, JSValueConst *argv)
{
    dyn_sqlite_t *s;
    const char *path = NULL;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, ro = 0, big = 0;
    JSValue v;
    (void)new_target;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "new SQLite(path, options?)");
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
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

    s = (dyn_sqlite_t *)calloc(1, sizeof(*s));
    if (!s) { JS_FreeCString(ctx, path); return JS_ThrowOutOfMemory(ctx); }
    s->ctx = ctx;
    s->bigint = big;
    s->cache_max = SQL_CACHE_DEFAULT;
    /* NOMUTEX: this handle is used from ONE thread (the JS thread). Saying so
     * skips SQLite's own locking; it also means a handle must never be shared
     * with the IO pool without changing this. */
    if (sqlite3_open_v2(path, &s->db, flags | SQLITE_OPEN_NOMUTEX,
                        NULL) != SQLITE_OK) {
        JSValue e = JS_ThrowInternalError(ctx, "SQLite: cannot open '%s': %s",
                                          path,
                                          s->db ? sqlite3_errmsg(s->db)
                                                : "out of memory");
        JS_FreeCString(ctx, path);
        if (s->db) sqlite3_close_v2(s->db);
        free(s);
        return e;
    }
    /* The authorizer is what makes `readonly` a promise SQLite itself
     * enforces, not just an open flag on the main database. */
    if (ro)
        sqlite3_set_authorizer(s->db, sqlite_ro_authorizer, NULL);
    JS_FreeCString(ctx, path);
    return dyn_res_wrap(ctx, dyn_sqlite_class_id, s, dyn_sqlite_dispose);
}

/* Bind a TypedArray, DataView or ArrayBuffer as a BLOB. Returns 1 if `v` was
 * one of those, 0 if it is some other object. SQLITE_TRANSIENT copies before
 * returning, so the buffer does not have to outlive this call. */
static int sqlite_bind_bytes(JSContext *ctx, sqlite3_stmt *st, int idx,
                             JSValueConst v, int *rc)
{
    size_t off = 0, len = 0, bpe = 0, total = 0;
    uint8_t *base;
    JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);

    if (JS_IsException(ab)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        base = JS_GetArrayBuffer(ctx, &total, v);      /* a bare ArrayBuffer */
        if (!base) { JS_FreeValue(ctx, JS_GetException(ctx)); return 0; }
        off = 0; len = total;
    } else {
        uint8_t *b = JS_GetArrayBuffer(ctx, &total, ab);
        JS_FreeValue(ctx, ab);
        if (!b) { JS_FreeValue(ctx, JS_GetException(ctx)); return 0; }
        base = b;
    }
    /* A NULL pointer binds SQL NULL, not an empty blob, so an empty view has
     * to hand sqlite something non-NULL to stay distinguishable from NULL. */
    if (len > 0x7fffffff) {
        JS_ThrowRangeError(ctx, "SQLite: blob parameter exceeds 2GiB");
        *rc = SQLITE_TOOBIG;
        return 1;
    }
    *rc = sqlite3_bind_blob(st, idx, len ? (const void *)(base + off) : "",
                            (int)len, SQLITE_TRANSIENT);
    return 1;
}

/* Bind argv[i] to parameter i+1. Returns 0, or -1 having thrown. */
static int sqlite_bind_one(JSContext *ctx, sqlite3_stmt *st, int idx,
                           JSValueConst v)
{
    int rc;
    if (JS_IsNull(v) || JS_IsUndefined(v)) {
        rc = sqlite3_bind_null(st, idx);
    } else if (JS_IsBool(v)) {
        rc = sqlite3_bind_int(st, idx, JS_ToBool(ctx, v) ? 1 : 0);
    } else if (JS_IsBigInt(ctx, v)) {
        int64_t n;
        /* Out of int64 range there is no lossless binding: SQLite's widest
         * integer IS int64, so refuse rather than wrap or round to a double. */
        if (JS_ToBigInt64(ctx, &n, v))
            return -1;
        rc = sqlite3_bind_int64(st, idx, n);
    } else if (JS_IsObject(v)) {
        if (!sqlite_bind_bytes(ctx, st, idx, v, &rc)) {
            /* Everything else stringifies cleanly and wrongly: {} stores as
             * "[object Object]", [1,2] as "1,2". Name the conversion. */
            JS_ThrowTypeError(ctx,
                "SQLite: parameter %d is an object; a parameter is a value. "
                "Pass a Uint8Array or ArrayBuffer for a BLOB, "
                "JSON.stringify(v) for JSON, or an ISO string for a date", idx);
            return -1;
        }
    } else if (JS_IsNumber(v)) {
        double d;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        /* An integral double binds as an integer so a round trip through the
         * column keeps its type; 1.0 and 1 are the same value to JS but not to
         * a schema with an INTEGER column. */
        if (d == (double)(int64_t)d)
            rc = sqlite3_bind_int64(st, idx, (int64_t)d);
        else
            rc = sqlite3_bind_double(st, idx, d);
    } else {
        size_t len;
        const char *str = JS_ToCStringLen(ctx, &len, v);
        if (!str)
            return -1;
        /* sqlite3_bind_text takes an int; a size_t past INT_MAX would wrap
         * negative and hand SQLite a nonsense length. */
        if (len > 0x7fffffff) {
            JS_FreeCString(ctx, str);
            JS_ThrowRangeError(ctx, "SQLite: text parameter exceeds 2GiB");
            return -1;
        }
        /* SQLITE_TRANSIENT: SQLite copies, because `str` is freed below. */
        rc = sqlite3_bind_text(st, idx, str, (int)len, SQLITE_TRANSIENT);
        JS_FreeCString(ctx, str);
    }
    if (rc != SQLITE_OK) {
        JS_ThrowInternalError(ctx, "SQLite: cannot bind parameter %d", idx);
        return -1;
    }
    return 0;
}

/* A BLOB as a Uint8Array. NOT a bare ArrayBuffer: that has no .length and no
 * indexing, so a caller's `for (i < b.length)` runs zero times and reports
 * success. Same shape the Redis client returns for a binary bulk reply. */
static JSValue sqlite_bytes(JSContext *ctx, const uint8_t *p, size_t n)
{
    JSValue ab = JS_NewArrayBufferCopy(ctx, p, n), ta;
    JSValueConst a3[3];
    if (JS_IsException(ab))
        return ab;
    /* three arguments: with one, the view's length defaults to 0 */
    a3[0] = ab; a3[1] = JS_NewInt32(ctx, 0); a3[2] = JS_NewInt32(ctx, (int)n);
    ta = JS_NewTypedArray(ctx, 3, a3, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return ta;
}

static JSValue sqlite_column(JSContext *ctx, dyn_sqlite_t *s, sqlite3_stmt *st,
                             int i)
{
    switch (sqlite3_column_type(st, i)) {
    case SQLITE_INTEGER: {
        sqlite3_int64 n = sqlite3_column_int64(st, i);
        /* Past 2^53 a double silently loses precision. `bigint` returns the
         * exact value with its type; without it the digits come back as text,
         * because a rounded number is worse than either. */
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
        const void *b = sqlite3_column_blob(st, i);
        int n = sqlite3_column_bytes(st, i);
        return sqlite_bytes(ctx, (const uint8_t *)(b ? b : ""),
                            (size_t)(n > 0 ? n : 0));
    }
    default: {
        const unsigned char *t = sqlite3_column_text(st, i);
        int n = sqlite3_column_bytes(st, i);
        return JS_NewStringLen(ctx, (const char *)(t ? t : (const unsigned char *)""),
                               (size_t)(n > 0 ? n : 0));
    }
    }
}

/* Does `p` hold nothing but semicolons and whitespace? The tail after the
 * first statement is then "no second statement", so a trailing ';' keeps the
 * single-statement fast path. */
static int sql_tail_empty(const char *p)
{
    while (*p) {
        if (*p == ';') { p++; continue; }
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { p++; continue; }
        return 0;
    }
    return 1;
}

/* Run every remaining statement of a multi-statement exec(). They take no
 * parameters -- a parameter array binds to the FIRST statement only, and
 * silently ignoring parameters a later statement declared would turn a WHERE
 * into a no-op. Returns accumulated changes, or -1 having thrown. */
static int64_t sql_run_rest(JSContext *ctx, dyn_sqlite_t *s, const char *tail)
{
    sqlite3_stmt *st = NULL;
    const char *p = tail;
    int64_t changes = 0;
    int rc;

    while (p && *p) {
        while (*p == ';') p++;
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
        while ((rc = sqlite3_step(st)) == SQLITE_ROW)
            continue;
        if (rc != SQLITE_DONE) {
            sqlite_throw(ctx, s, "step failed");
            sqlite3_finalize(st);
            return -1;
        }
        changes += sqlite3_changes(s->db);
        sqlite3_finalize(st);
    }
    return changes;
}

/* Prepare, bind and step. `want_rows` selects query() from exec().
 *
 * query() runs exactly ONE statement: the first, refusing input whose tail
 * carries a second one rather than returning rows whose shape changes partway
 * through. exec() runs ALL of them. Repeated single-statement text hits the
 * prepared-statement cache and skips parse+plan entirely. */
static JSValue sqlite_run(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv, int want_rows)
{
    dyn_sqlite_t *s;
    sqlite3_stmt *st = NULL;
    const char *sql;
    size_t sqllen;
    uint32_t hash;
    sql_cache_ent_t *ent;
    int cached = 0;
    const char *tail = NULL;
    JSValue rows = JS_UNDEFINED;
    uint32_t nrow = 0;
    int rc, i, nparam = 0;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "sql string required");
    sql = JS_ToCStringLen(ctx, &sqllen, argv[0]);
    if (!sql)
        return JS_EXCEPTION;
    s = (dyn_sqlite_t *)dyn_res_native(ctx, this_val, dyn_sqlite_class_id);
    if (!s) { JS_FreeCString(ctx, sql); return JS_EXCEPTION; }

    hash = sql_hash(sql, sqllen);
    ent = sql_cache_find(s, sql, sqllen, hash);
    if (ent) {
        /* Reset FIRST: a cached statement must never be entered mid-use, and
         * reset also clears any error state a previous run left behind. */
        st = ent->st;
        sqlite3_reset(st);
        cached = 1;
    } else {
        if (sqlite3_prepare_v2(s->db, sql, -1, &st, &tail) != SQLITE_OK) {
            JSValue e = sqlite_throw(ctx, s, "prepare failed");
            JS_FreeCString(ctx, sql);
            return e;
        }
        if (!st) {                      /* "", ";" -- nothing to run */
            JS_FreeCString(ctx, sql);
            return want_rows ? JS_NewArray(ctx) : JS_NewInt32(ctx, 0);
        }
        if (want_rows && !sql_tail_empty(tail)) {
            sqlite3_finalize(st);
            JS_FreeCString(ctx, sql);
            return JS_ThrowRangeError(ctx,
                "SQLite: query runs one statement; this text carries a "
                "second -- split them or use exec()");
        }
    }
    JS_FreeCString(ctx, sql);

    if (argc > 1 && JS_IsArray(ctx, argv[1])) {
        JSValue lenv = JS_GetPropertyStr(ctx, argv[1], "length");
        int64_t n = 0;
        /* The return IS checked: an unchecked conversion left nparam = 0, so a
           failed read surfaced as the misleading "takes %d parameters, 0
           given" instead of the real conversion exception. */
        if (JS_ToInt64(ctx, &n, lenv)) {
            JS_FreeValue(ctx, lenv);
            if (cached)
                sqlite3_reset(st);
            else
                sqlite3_finalize(st);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lenv);
        nparam = (int)n;
        /* Refuse a mismatch rather than leaving parameters NULL: a silently
         * unbound parameter turns a WHERE into a no-op. The PLAN is not in
         * question, so the cached entry stays -- a reset returns it to
         * pristine and the next call pays nothing. */
        if (nparam != sqlite3_bind_parameter_count(st)) {
            int want = sqlite3_bind_parameter_count(st);
            if (cached)
                sqlite3_reset(st);
            else
                sqlite3_finalize(st);
            return JS_ThrowRangeError(ctx,
                "SQLite: statement takes %d parameters, %d given", want, nparam);
        }
        for (i = 0; i < nparam; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i);
            int bad = sqlite_bind_one(ctx, st, i + 1, v);
            JS_FreeValue(ctx, v);
            if (bad) {
                if (cached) sqlite3_reset(st);
                else sqlite3_finalize(st);
                return JS_EXCEPTION;
            }
        }
    } else if (sqlite3_bind_parameter_count(st) > 0) {
        int want = sqlite3_bind_parameter_count(st);
        if (cached) sqlite3_reset(st);
        else sqlite3_finalize(st);
        return JS_ThrowRangeError(ctx,
            "SQLite: statement takes %d parameters, none given", want);
    }

    /* optional { maxRows } bound: a runaway SELECT materialises memory on
     * the JS thread until OOM without one */
    int64_t max_rows = 0;
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue mv = JS_GetPropertyStr(ctx, argv[2], "maxRows");
        if (JS_IsException(mv))
            goto fail;
        if (!JS_IsUndefined(mv) && !JS_IsNull(mv)) {
            int64_t mr = 0;
            if (JS_ToInt64(ctx, &mr, mv) || mr < 0) {
                JS_FreeValue(ctx, mv);
                JS_ThrowRangeError(ctx, "SQLite: maxRows must be >= 0");
                goto fail;
            }
            max_rows = mr;
        }
        JS_FreeValue(ctx, mv);
    }

    if (want_rows) {
        rows = JS_NewArray(ctx);
        if (JS_IsException(rows)) goto fail;
    }
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
            if (JS_IsException(o)) goto fail;
            for (i = 0; i < ncol; i++) {
                /* DEFINE, not set: JS_SetPropertyStr walks the prototype
                 * chain, so a column named __proto__ retargets the row's
                 * prototype and vanishes from the result. */
                const char *cn = sqlite3_column_name(st, i);
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
        /* An error leaves the plan suspect (a schema may have moved under
         * it); evicting costs one re-prepare and cannot compound. */
        if (cached) { sql_cache_drop_stmt(s, st); sqlite3_finalize(st); }
        else sqlite3_finalize(st);
        if (want_rows)
            JS_FreeValue(ctx, rows);
        return e;
    }

    if (cached) {
        sqlite3_reset(st);          /* clean for the next hit */
    } else {
        if (!want_rows && tail && !sql_tail_empty(tail)) {
            /* Multi-statement exec: run the rest, summing changes. NOT cached
             * -- a cache key is the full text, and a hit must never mean
             * "run only the first statement again". */
            int64_t first = sqlite3_changes(s->db);
            int64_t more;
            sqlite3_finalize(st);
            more = sql_run_rest(ctx, s, tail);
            if (more < 0)
                return JS_EXCEPTION;
            return JS_NewInt64(ctx, first + more);
        }
        if (tail && sql_tail_empty(tail))
            sql_cache_put(s, sqlite3_sql(st), sqllen, hash, st);  /* cache owns it */
        else
            sqlite3_finalize(st);
    }
    if (want_rows)
        return rows;
    return JS_NewInt32(ctx, sqlite3_changes(s->db));

fail:
    /* A JS exception mid-loop leaves the statement mid-iteration, not
     * suspect: reset returns it to pristine and the cache keeps it. */
    if (cached)
        sqlite3_reset(st);
    else
        sqlite3_finalize(st);
    JS_FreeValue(ctx, rows);
    return JS_EXCEPTION;
}

static JSValue dyn_sqlite_query(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv)
{ return sqlite_run(ctx, this_val, argc, argv, 1); }

static JSValue dyn_sqlite_exec(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv)
{ return sqlite_run(ctx, this_val, argc, argv, 0); }

static JSValue dyn_sqlite_last_id(JSContext *ctx, JSValueConst this_val)
{
    dyn_sqlite_t *s = (dyn_sqlite_t *)dyn_res_native(ctx, this_val,
                                                     dyn_sqlite_class_id);
    if (!s) return JS_EXCEPTION;
    return JS_NewInt64(ctx, sqlite3_last_insert_rowid(s->db));
}

static JSValue dyn_sqlite_version(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    /* The LINKED library's version, read at runtime. The header's
     * SQLITE_VERSION would report what we compiled against, which on a system
     * with several copies is not what is loaded. */
    return JS_NewString(ctx, sqlite3_libversion());
}

static const JSCFunctionListEntry dyn_sqlite_proto[] = {
    JS_CFUNC_DEF("query", 1, dyn_sqlite_query),
    JS_CFUNC_DEF("exec", 1, dyn_sqlite_exec),
    JS_CGETSET_DEF("lastInsertRowId", dyn_sqlite_last_id, NULL),
    JS_CGETSET_DEF("version", dyn_sqlite_version, NULL),
};

int dyn_sqlite_register(JSContext *ctx, JSModuleDef *m)
{
    return dyn_register_class(ctx, m, &dyn_sqlite_class_id, &dyn_sqlite_class,
                              dyn_sqlite_proto, countof(dyn_sqlite_proto),
                              dyn_sqlite_ctor, "SQLite");
}

void dyn_sqlite_add_exports(JSContext *ctx, JSModuleDef *m)
{
    JS_AddModuleExport(ctx, m, "SQLite");
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_NET && CONFIG_SQLITE */
