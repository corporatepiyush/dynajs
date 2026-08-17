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
    JSContext *ctx;
    sqlite3 *db;
    int bigint;             /* 64-bit integers as BigInt rather than text */
} dyn_sqlite_t;

static JSClassID dyn_sqlite_class_id;
static const JSClassDef dyn_sqlite_class = {
    "SQLite", .finalizer = dyn_res_finalizer,
};

static void dyn_sqlite_dispose(void *native)
{
    dyn_sqlite_t *s = (dyn_sqlite_t *)native;
    if (!s)
        return;
    if (s->db)
        sqlite3_close_v2(s->db);   /* _v2 tolerates unfinalised statements */
    free(s);
}

static JSValue sqlite_throw(JSContext *ctx, dyn_sqlite_t *s, const char *what)
{
    return JS_ThrowInternalError(ctx, "SQLite: %s: %s", what,
                                 s->db ? sqlite3_errmsg(s->db) : "no database");
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

/* Prepare, bind and step. `want_rows` selects query() from exec(). */
static JSValue sqlite_run(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv, int want_rows)
{
    dyn_sqlite_t *s;
    sqlite3_stmt *st = NULL;
    const char *sql;
    JSValue rows = JS_UNDEFINED;
    uint32_t nrow = 0;
    int rc, i, nparam = 0;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "sql string required");
    sql = JS_ToCString(ctx, argv[0]);
    if (!sql)
        return JS_EXCEPTION;
    s = (dyn_sqlite_t *)dyn_res_native(ctx, this_val, dyn_sqlite_class_id);
    if (!s) { JS_FreeCString(ctx, sql); return JS_EXCEPTION; }

    if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) != SQLITE_OK) {
        JSValue e = sqlite_throw(ctx, s, "prepare failed");
        JS_FreeCString(ctx, sql);
        return e;
    }
    JS_FreeCString(ctx, sql);

    if (argc > 1 && JS_IsArray(ctx, argv[1])) {
        JSValue lenv = JS_GetPropertyStr(ctx, argv[1], "length");
        int64_t n = 0;
        JS_ToInt64(ctx, &n, lenv);
        JS_FreeValue(ctx, lenv);
        nparam = (int)n;
        /* Refuse a mismatch rather than leaving parameters NULL: a silently
         * unbound parameter turns a WHERE into a no-op. */
        if (nparam != sqlite3_bind_parameter_count(st)) {
            int want = sqlite3_bind_parameter_count(st);
            sqlite3_finalize(st);
            return JS_ThrowRangeError(ctx,
                "SQLite: statement takes %d parameters, %d given", want, nparam);
        }
        for (i = 0; i < nparam; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i);
            int bad = sqlite_bind_one(ctx, st, i + 1, v);
            JS_FreeValue(ctx, v);
            if (bad) { sqlite3_finalize(st); return JS_EXCEPTION; }
        }
    } else if (sqlite3_bind_parameter_count(st) > 0) {
        int want = sqlite3_bind_parameter_count(st);
        sqlite3_finalize(st);
        return JS_ThrowRangeError(ctx,
            "SQLite: statement takes %d parameters, none given", want);
    }

    if (want_rows) {
        rows = JS_NewArray(ctx);
        if (JS_IsException(rows)) { sqlite3_finalize(st); return rows; }
    }
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (!want_rows)
            continue;
        {
            int ncol = sqlite3_column_count(st);
            JSValue o = JS_NewObject(ctx);
            if (JS_IsException(o)) { sqlite3_finalize(st); JS_FreeValue(ctx, rows); return o; }
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
        sqlite3_finalize(st);
        JS_FreeValue(ctx, rows);
        return e;
    }
    sqlite3_finalize(st);
    if (want_rows)
        return rows;
    return JS_NewInt32(ctx, sqlite3_changes(s->db));
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
