#include "postgres-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_POSTGRES)
void pg_stmt_learn_oids(dyn_pg_t* g, const char* name,
    const uint32_t* oids, int n)
{
    int i;
    if (!name || !name[0] || n <= 0)
        return;
    for (i = 0; i < g->nstmts; i++) {
        struct pg_stmt* s = &g->stmts[i];
        if (strcmp(s->name, name) != 0)
            continue;
        if (s->oids)
            return;
        s->oids = (uint32_t*)malloc((size_t)n * sizeof(*s->oids));
        if (!s->oids)
            return;
        memcpy(s->oids, oids, (size_t)n * sizeof(*s->oids));
        s->noids = n;
        return;
    }
}

static uint32_t pg_stmt_hash(const char* s, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++)
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static struct pg_stmt* pg_stmt_find(dyn_pg_t* g, const char* sql, size_t n,
    uint32_t h)
{
    int i;
    for (i = 0; i < g->nstmts; i++)
        if (g->stmts[i].hash == h && g->stmts[i].sqllen == n && memcmp(g->stmts[i].sql, sql, n) == 0)
            return &g->stmts[i];
    return NULL;
}

void pg_stmt_evict_named(dyn_pg_t* g, const char* name)
{
    int i;
    if (!name || !name[0])
        return;
    for (i = 0; i < g->nstmts; i++)
        if (strcmp(g->stmts[i].name, name) == 0) {
            free(g->stmts[i].sql);
            free(g->stmts[i].oids);
            g->stmts[i] = g->stmts[--g->nstmts];
            return;
        }
}

void pg_stmt_clear(dyn_pg_t* g)
{
    int i;
    for (i = 0; i < g->nstmts; i++) {
        free(g->stmts[i].sql);
        free(g->stmts[i].oids);
    }
    free(g->stmts);
    g->stmts = NULL;
    g->nstmts = g->cap_stmts = 0;
}

struct pg_stmt* pg_stmt_get(dyn_pg_t* g, const char* sql, size_t n)
{
    uint32_t h;
    struct pg_stmt* s;

    if (g->stmt_cache_max <= 0)
        return NULL;
    h = pg_stmt_hash(sql, n);
    s = pg_stmt_find(g, sql, n, h);
    if (s) {
        if (s->uses < 1000000)
            s->uses++;
        return s;
    }
    if (g->nstmts >= g->stmt_cache_max)
        return NULL;
    if (g->nstmts == g->cap_stmts) {
        int cap = g->cap_stmts ? g->cap_stmts * 2 : 8;
        struct pg_stmt* ns = (struct pg_stmt*)realloc(g->stmts,
            (size_t)cap * sizeof(*ns));
        if (!ns)
            return NULL;
        g->stmts = ns;
        g->cap_stmts = cap;
    }
    s = &g->stmts[g->nstmts];
    memset(s, 0, sizeof(*s));
    s->sql = (char*)malloc(n + 1);
    if (!s->sql)
        return NULL;
    memcpy(s->sql, sql, n);
    s->sql[n] = '\0';
    s->sqllen = n;
    s->hash = h;
    s->uses = 1;
    snprintf(s->name, sizeof(s->name), "djs%u", ++g->stmt_seq);
    g->nstmts++;
    return s;
}

JSValue dyn_pg_get_stmt_stats(JSContext* ctx, JSValueConst this_val)
{
    dyn_pg_t* g = pg_this(ctx, this_val);
    JSValue o;
    if (!g)
        return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, o, "size", JS_NewInt32(ctx, g->nstmts),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "max", JS_NewInt32(ctx, g->stmt_cache_max),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "prepareAfter",
        JS_NewInt32(ctx, g->prepare_after), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "preparedHits",
        JS_NewInt64(ctx, (int64_t)g->n_prepared_hits), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "unnamed",
        JS_NewInt64(ctx, (int64_t)g->n_unnamed), JS_PROP_C_W_E);
    return o;
}
#endif
