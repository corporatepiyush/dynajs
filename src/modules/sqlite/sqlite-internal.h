#ifndef SQLITE_INTERNAL_H
#define SQLITE_INTERNAL_H

#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SQLITE) && defined(CONFIG_SQLITE)

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

extern JSClassID dyn_sqlite_class_id;

sql_cache_ent_t* sql_cache_find(dyn_sqlite_t* s, const char* sql,
    size_t n, uint32_t h);

int sql_cache_put(dyn_sqlite_t* s, const char* sql, size_t n,
    uint32_t h, sqlite3_stmt* st);

void sql_cache_drop_stmt(dyn_sqlite_t* s, sqlite3_stmt* st);

void sql_cache_clear(dyn_sqlite_t* s);

JSValue sqlite_run(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int want_rows);

JSValue sqlite_run_many(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

int sqlite_progress(void* ud);

#define SQL_PROGRESS_OPS 10000

#endif

#endif
