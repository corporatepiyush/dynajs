#include "postgres-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_POSTGRES)
void pg_settle(dyn_pg_t* g, dyn_pg_pending_t* p, int reject, JSValue v)
{
    JSContext* ctx = g->ctx;
    JSValue fn = reject ? p->reject : p->resolve;
    if (p->copy)
        pg_copy_settled(ctx, p->copy, reject, v);
    if (JS_IsFunction(ctx, fn)) {
        JSValueConst a[1] = { v };
        g->cb_depth++;
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, a);
        g->cb_depth--;
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, v);
    pgp_free(ctx, p);
    pg_release_if_idle(g);
}

static void pg_batch_assemble(dyn_pg_t* g, dyn_pg_batch_t* b)
{
    JSContext* ctx = b->ctx;
    JSValue arr = JS_NewArray(ctx);
    int i;

    if (JS_IsException(arr)) {
        for (i = 0; i < b->k; i++)
            JS_FreeValue(ctx, b->slots[i]);
        JS_FreeValue(ctx, JS_GetException(ctx));
        arr = pg_conn_error(ctx, "PostgreSQL: out of memory assembling the pipeline result");
        if (JS_IsFunction(ctx, b->reject)) {
            JSValueConst a[1] = { arr };
            g->cb_depth++;
            JSValue r = JS_Call(ctx, b->reject, JS_UNDEFINED, 1, a);
            g->cb_depth--;
            JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, arr);
    } else {
        for (i = 0; i < b->k; i++)
            JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, b->slots[i],
                JS_PROP_C_W_E);
        if (JS_IsFunction(ctx, b->resolve)) {
            JSValueConst a[1] = { arr };
            g->cb_depth++;
            JSValue r = JS_Call(ctx, b->resolve, JS_UNDEFINED, 1, a);
            g->cb_depth--;
            JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, arr);
    }
}

static void pg_batch_fill(dyn_pg_t* g, dyn_pg_pending_t* p, JSValue v)
{
    dyn_pg_batch_t* b = p->batch;

    p->batch = NULL;
    if (!b || b->dead || b->settled) {
        JS_FreeValue(g->ctx, v);
        return;
    }
    b->slots[p->bidx] = v;
    if (++b->ndone == b->k) {
        b->settled = 1;
        pg_batch_assemble(g, b);
        JS_FreeValue(b->ctx, b->resolve);
        JS_FreeValue(b->ctx, b->reject);
        free(b->slots);
        free(b);
    }
}

void pg_batch_fail(dyn_pg_t* g, dyn_pg_batch_t* b, const char* msg)
{
    JSContext* ctx = b->ctx;
    dyn_pg_pending_t* p;
    JSValue e;
    int i;

    if (b->dead || b->settled)
        return;
    b->dead = 1;
    for (p = g->head; p; p = p->next)
        if (p->batch == b)
            p->batch = NULL;
    for (p = g->dhead; p; p = p->next)
        if (p->batch == b)
            p->batch = NULL;
    for (p = g->wq_head; p; p = p->next)
        if (p->batch == b)
            p->batch = NULL;
    if (pg_in_final) {
        for (i = 0; i < b->k; i++)
            JS_FreeValueRT(g->rt, b->slots[i]);
        free(b->slots);
        JS_FreeValueRT(g->rt, b->resolve);
        JS_FreeValueRT(g->rt, b->reject);
        free(b);
        return;
    }
    for (i = 0; i < b->k; i++)
        JS_FreeValue(ctx, b->slots[i]);
    free(b->slots);
    e = pg_conn_error(ctx, msg);
    if (JS_IsFunction(ctx, b->reject)) {
        JSValueConst a[1] = { e };
        g->cb_depth++;
        JSValue r = JS_Call(ctx, b->reject, JS_UNDEFINED, 1, a);
        g->cb_depth--;
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, b->resolve);
    JS_FreeValue(ctx, b->reject);
    free(b);
}

static int32_t pg_tag_rowcount(const char* tag)
{
    const char* sp = strrchr(tag, ' ');
    const char* d;
    long v = 0;

    if (!sp || !sp[1])
        return -1;
    for (d = sp + 1; *d; d++) {
        if (*d < '0' || *d > '9')
            return -1;
        v = v * 10 + (*d - '0');
        if (v > INT32_MAX)
            return INT32_MAX;
    }
    return (int32_t)v;
}

void pg_settle_entry(dyn_pg_t* g, dyn_pg_pending_t* p)
{
    JSContext* ctx = g->ctx;
    int32_t affected;

    if (!JS_IsUndefined(p->error)) {
        JSValue e = p->error;
        p->error = JS_UNDEFINED;
        pg_settle_entry_err(g, p, e);
        return;
    }
    {
        int32_t t = p->rowcount > 0 ? p->rowcount : pg_tag_rowcount(p->tag);
        affected = t < 0 ? 0 : t;
    }

    if (p->batch) {
        JSValue res = JS_NewObject(ctx);
        if (JS_IsException(res)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            res = pg_conn_error(ctx,
                "PostgreSQL: out of memory building a pipeline result");
        } else {
            JS_DefinePropertyValueStr(ctx, res, "rows",
                JS_IsUndefined(p->rows) ? JS_NewArray(ctx)
                                        : p->rows,
                JS_PROP_C_W_E);
            p->rows = JS_UNDEFINED;
            JS_DefinePropertyValueStr(ctx, res, "fields",
                JS_IsUndefined(p->fields) ? JS_NewArray(ctx)
                                          : p->fields,
                JS_PROP_C_W_E);
            p->fields = JS_UNDEFINED;
            JS_DefinePropertyValueStr(ctx, res, "command",
                JS_NewString(ctx, p->tag), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, res, "rowCount",
                JS_NewInt32(ctx, affected), JS_PROP_C_W_E);
        }
        pgp_free_fields(ctx, p);
        free(p->bytes);
        p->bytes = NULL;
        pg_batch_fill(g, p, res);
        pgp_free(ctx, p);
        return;
    }

    {
        JSValue res = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, res, "rows",
            JS_IsUndefined(p->rows) ? JS_NewArray(ctx) : p->rows,
            JS_PROP_C_W_E);
        p->rows = JS_UNDEFINED;
        JS_DefinePropertyValueStr(ctx, res, "fields",
            JS_IsUndefined(p->fields) ? JS_NewArray(ctx) : p->fields,
            JS_PROP_C_W_E);
        p->fields = JS_UNDEFINED;
        JS_DefinePropertyValueStr(ctx, res, "command",
            JS_NewString(ctx, p->tag), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, res, "rowCount",
            JS_NewInt32(ctx, affected), JS_PROP_C_W_E);
        pg_settle(g, p, 0, res);
    }
}

void pg_settle_entry_err(dyn_pg_t* g, dyn_pg_pending_t* p, JSValue e)
{
    JSContext* ctx = g->ctx;

    if (p->batch) {
        pgp_free_fields(ctx, p);
        free(p->bytes);
        p->bytes = NULL;
        pg_batch_fill(g, p, e);
        pgp_free(ctx, p);
        return;
    }
    pg_settle(g, p, 1, e);
}

JSValue dyn_pg_pipeline(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_pg_t* g;
    uint32_t n = 0, i, coerced = 0, encoded = 0, submitted = 0;
    JSValue lv, promise;
    JSValue funcs[2];
    pgw_t* ws = NULL;
    char (*names)[24];
    const char** sqls = NULL;
    const char*** pvs = NULL;
    size_t** plens = NULL;
    uint8_t **pnulls = NULL, **pfrees = NULL;
    uint32_t* nparams = NULL;
    dyn_pg_batch_t* b;
    dyn_pg_pending_t* p;
    uint64_t esync;
    int sub_rc;

    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx,
            "pipeline: expects an array of [sql, params?] elements");
    lv = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_ToUint32(ctx, &n, lv) < 0) {
        JS_FreeValue(ctx, lv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lv);
    if (n == 0)
        return JS_ThrowTypeError(ctx, "pipeline: at least one statement");
    if (n > 65535)
        return JS_ThrowRangeError(ctx, "pipeline: at most 65535 statements");

    sqls = (const char**)calloc(n, sizeof(*sqls));
    pvs = (const char***)calloc(n, sizeof(*pvs));
    plens = (size_t**)calloc(n, sizeof(*plens));
    pnulls = (uint8_t**)calloc(n, sizeof(*pnulls));
    pfrees = (uint8_t**)calloc(n, sizeof(*pfrees));
    nparams = (uint32_t*)calloc(n, sizeof(*nparams));
    ws = (pgw_t*)calloc(n, sizeof(*ws));
    names = (char (*)[24])calloc(n, sizeof(*names));
    if (!sqls || !pvs || !plens || !pnulls || !pfrees || !nparams || !ws || !names)
        goto oom_pre;

#define PGPIPE_MEMBER_FREE(k)                                         \
    do {                                                              \
        if (sqls[k])                                                  \
            JS_FreeCString(ctx, sqls[k]);                             \
        pg_free_params(ctx, pvs[k], pfrees[k], nparams[k]);           \
                                                                      \
        pg_param_arrays_free(pvs[k], plens[k], pnulls[k], pfrees[k]); \
        free(ws[k].b);                                                \
        ws[k].b = NULL;                                               \
    } while (0)

    for (i = 0; i < n; i++) {
        JSValue el = JS_GetPropertyUint32(ctx, argv[0], i);
        JSValue sqlv, par;
        uint32_t m = 0;

        if (JS_IsException(el))
            goto fail_coerced;
        if (!JS_IsArray(ctx, el)) {
            JS_FreeValue(ctx, el);
            JS_ThrowTypeError(ctx, "pipeline: element %u is not [sql, params?]",
                i);
            goto fail_coerced;
        }
        {
            JSValue l2 = JS_GetPropertyStr(ctx, el, "length");
            int rc = JS_ToUint32(ctx, &m, l2);
            JS_FreeValue(ctx, l2);
            if (rc < 0) {
                JS_FreeValue(ctx, el);
                goto fail_coerced;
            }
        }
        if (m < 1) {
            JS_FreeValue(ctx, el);
            JS_ThrowTypeError(ctx, "pipeline: element %u is empty", i);
            goto fail_coerced;
        }
        sqlv = JS_GetPropertyUint32(ctx, el, 0);
        {
            size_t slen = 0;
            sqls[i] = JS_ToCStringLen(ctx, &slen, sqlv);
        }
        JS_FreeValue(ctx, sqlv);
        if (!sqls[i]) {
            JS_FreeValue(ctx, el);
            goto fail_coerced;
        }
        par = m > 1 ? JS_GetPropertyUint32(ctx, el, 1) : JS_UNDEFINED;
        if (JS_IsException(par)) {
            JS_FreeValue(ctx, el);
            goto fail_coerced;
        }
        if (!JS_IsUndefined(par) && !JS_IsNull(par)) {
            if (pg_coerce_params(ctx, par, &pvs[i], &plens[i], &pnulls[i],
                    &pfrees[i], &nparams[i], "pipeline")
                < 0) {
                JS_FreeValue(ctx, par);
                JS_FreeValue(ctx, el);
                goto fail_coerced;
            }
        }
        JS_FreeValue(ctx, par);
        JS_FreeValue(ctx, el);
        coerced = i + 1;
    }

    g = pg_this(ctx, this_val);
    if (!g || g->state == PG_ST_DEAD) {
        JS_ThrowInternalError(ctx, "PostgreSQL: the connection is closed");
        goto fail_ready;
    }
    if ((uint64_t)g->npending + g->nwait + n > (uint64_t)g->maxpending) {
        JS_ThrowInternalError(ctx,
            "PostgreSQL: %d queries already in flight (maxPending)",
            g->maxpending);
        goto fail_ready;
    }

    for (i = 0; i < n; i++) {
        if (pg_encode_member(g, &ws[i], sqls[i], (int)nparams[i], pvs[i],
                plens[i], pnulls[i], names[i], sizeof(names[0]))
            < 0)
            break;
        encoded = i + 1;
        if (i == n - 1) {
            size_t at = pgw_begin(&ws[i], 'S');
            pgw_end(&ws[i], at);
        }
    }
    if (encoded < n) {
        JS_ThrowOutOfMemory(ctx);
        goto fail_ready;
    }

    b = (dyn_pg_batch_t*)calloc(1, sizeof(*b));
    if (!b) {
        JS_ThrowOutOfMemory(ctx);
        goto fail_ready;
    }
    b->ctx = ctx;
    b->slots = (JSValue*)calloc(n, sizeof(*b->slots));
    if (!b->slots) {
        free(b);
        b = NULL;
        JS_ThrowOutOfMemory(ctx);
        goto fail_ready;
    }
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        free(b->slots);
        free(b);
        goto fail_ready;
    }
    b->resolve = funcs[0];
    b->reject = funcs[1];
    b->k = (int)n;
    for (i = 0; i < n; i++)
        b->slots[i] = JS_UNDEFINED;

    esync = ++g->sync_seq;
    for (i = 0; i < n; i++) {
        p = pgp_new();
        if (!p)
            break;
        snprintf(p->stmt_name, sizeof(p->stmt_name), "%s", names[i]);
        p->batch = b;
        p->bidx = (int)i;
        sub_rc = pg_submit(g, p, ws[i].b, ws[i].len, esync, i == n - 1);
        ws[i].b = NULL;
        if (sub_rc < 0) {
            free(p->bytes);
            p->bytes = NULL;
            pgp_free(ctx, p);
            break;
        }
        submitted = i + 1;
    }
    if (submitted < n) {
        if (!b->dead && !b->settled)
            pg_batch_fail(g, b,
                "PostgreSQL: out of memory queueing the pipeline");
        pg_fail_all(g,
            "PostgreSQL: out of memory queueing the pipeline; the submitted "
            "prefix has no Sync, so the connection is unusable");
    }

    if (g->state == PG_ST_READY)
        pg_flush_soon(g);
    pg_hold(ctx, this_val, g);
    for (i = 0; i < n; i++)
        PGPIPE_MEMBER_FREE(i);
    free(sqls);
    free(pvs);
    free(plens);
    free(pnulls);
    free(pfrees);
    free(nparams);
    free(ws);
    free(names);
    return promise;

fail_coerced:
    {
        uint32_t k;
        for (k = 0; k < n && k <= coerced; k++)
            PGPIPE_MEMBER_FREE(k);
    }
    free(sqls);
    free(pvs);
    free(plens);
    free(pnulls);
    free(pfrees);
    free(nparams);
    free(ws);
    free(names);
    return JS_EXCEPTION;

fail_ready:
    for (i = 0; i < coerced; i++)
        PGPIPE_MEMBER_FREE(i);
    free(sqls);
    free(pvs);
    free(plens);
    free(pnulls);
    free(pfrees);
    free(nparams);
    free(ws);
    free(names);
    return JS_EXCEPTION;

oom_pre:
    free(sqls);
    free(pvs);
    free(plens);
    free(pnulls);
    free(pfrees);
    free(nparams);
    free(ws);
    free(names);
    return JS_ThrowOutOfMemory(ctx);

#undef PGPIPE_MEMBER_FREE
}
#endif
