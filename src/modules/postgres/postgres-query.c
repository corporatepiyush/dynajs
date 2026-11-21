#include "postgres-internal.h"
#include "core/dyn-pgwire.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_POSTGRES)
static const char* const pg_query_keys[] = { "maxRows" };

static int pg_oid_prefers_binary(const dyn_pg_t* g, uint32_t oid)
{
    switch (oid) {
    case PG_OID_NUMERIC:
        return JS_IsFunction(g->ctx, g->numeric_ctor);
    case PG_OID_DATE:
    case PG_OID_TIMESTAMP:
    case PG_OID_TIMESTAMPTZ:
    case PG_OID_INTERVAL:
        return g->dates_native;
    case PG_OID_BOOL:
    case PG_OID_BYTEA:
    case PG_OID_INT8:
    case PG_OID_INT2:
    case PG_OID_INT4:
    case PG_OID_OID:
    case PG_OID_FLOAT4:
    case PG_OID_FLOAT8:
    case PG_OID_UUID:
        return 1;
    default: {
        uint32_t e = dyn_pgw_array_elem(oid), r = dyn_pgw_multirange_range(oid), s = dyn_pgw_range_sub(r ? r : oid);
        if (s)
            return g->ranges_obj && pg_oid_prefers_binary(g, s);
        return e && g->arrays_native && pg_oid_prefers_binary(g, e);
    }
    }
}

static void pgw_u16(pgw_t* w, uint16_t v)
{
    uint8_t t[2];
    t[0] = (uint8_t)(v >> 8);
    t[1] = (uint8_t)v;
    pgw_raw(w, t, 2);
}

static uint64_t pgq_seq;

static const char* const pgq_opts_keys[] = { "batch", "maxRows" };

JSValue dyn_pg_queryiter(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    pgq_iter_t* it;
    JSValue obj = JS_UNDEFINED, qfn;
    const char* sql;
    int64_t batch = 1000, max_rows = 0;

    if (argc > 2 && dyn_opts_strict(ctx, argv[2], pgq_opts_keys, 2))
        return JS_EXCEPTION;
    {
        size_t sql_len = 0;
        sql = JS_ToCStringLen(ctx, &sql_len, argv[0]);
        if (!sql)
            return JS_EXCEPTION;
        if (strlen(sql) != sql_len) {
            JS_FreeCString(ctx, sql);
            return JS_ThrowTypeError(ctx,
                "PostgreSQL: the SQL text contains U+0000; the protocol would cut it there");
        }
    }
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]) && !JS_IsArray(ctx, argv[1])) {
        JS_FreeCString(ctx, sql);
        return JS_ThrowTypeError(ctx,
            "queryIter: parameters must be an array");
    }
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue v;
        v = JS_GetPropertyStr(ctx, argv[2], "batch");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (JS_ToInt64(ctx, &batch, v)) {
                JS_FreeValue(ctx, v);
                JS_FreeCString(ctx, sql);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "maxRows");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (JS_ToInt64(ctx, &max_rows, v)) {
                JS_FreeValue(ctx, v);
                JS_FreeCString(ctx, sql);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
    }
    if (batch < 1)
        batch = 1;
    if (max_rows < 0) {
        JS_FreeCString(ctx, sql);
        return JS_ThrowRangeError(ctx, "queryIter: maxRows must be >= 0");
    }

    qfn = JS_GetPropertyStr(ctx, this_val, "query");
    if (JS_IsException(qfn)) {
        JS_FreeCString(ctx, sql);
        return JS_EXCEPTION;
    }
    if (!JS_IsFunction(ctx, qfn)) {
        JS_FreeValue(ctx, qfn);
        JS_FreeCString(ctx, sql);
        return JS_ThrowTypeError(ctx,
            "queryIter: receiver has no query() method");
    }

    it = (pgq_iter_t*)calloc(1, sizeof(*it));
    if (!it) {
        JS_FreeValue(ctx, qfn);
        JS_FreeCString(ctx, sql);
        return JS_ThrowOutOfMemory(ctx);
    }
    it->ctx = ctx;
    it->conn_obj = JS_DupValue(ctx, this_val);
    it->query_fn = qfn;
    {
        size_t slen = strlen(sql);
        it->sql = (char*)malloc(slen + 1);
        if (!it->sql) {
            JS_FreeValue(ctx, qfn);
            JS_FreeCString(ctx, sql);
            free(it);
            return JS_ThrowOutOfMemory(ctx);
        }
        memcpy(it->sql, sql, slen + 1);
    }
    JS_FreeCString(ctx, sql);
    it->params = (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]))
        ? JS_DupValue(ctx, argv[1])
        : JS_UNDEFINED;
    it->rows = JS_UNDEFINED;
    it->error = JS_UNDEFINED;
    it->batch = batch;
    it->max_rows = max_rows;
    it->state = PGQ_ST_IDLE;
    snprintf(it->name, sizeof(it->name), "_djq%llu",
        (unsigned long long)++pgq_seq);

    obj = JS_NewObjectClass(ctx, (int)pgq_class_id);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, it->conn_obj);
        JS_FreeValue(ctx, it->query_fn);
        JS_FreeValue(ctx, it->params);
        JS_FreeValue(ctx, it->rows);
        JS_FreeValue(ctx, it->error);
        free(it->sql);
        free(it);
        return obj;
    }
    JS_SetOpaque(obj, it);
    return obj;
}

static int pg_encode_query(pgw_t* w, const char* sql, int nparam,
    const char** pv, const size_t* plen,
    const uint8_t* pnull,
    const char* stmt_name, int stmt_prepared,
    const uint8_t* rfmt, int nrfmt)
{
    int i;
    if (!stmt_prepared) {
        size_t at = pgw_begin(w, 'P');
        pgw_str(w, stmt_name ? stmt_name : "");
        pgw_str(w, sql);
        pgw_u16(w, 0);
        pgw_end(w, at);
    }
    {
        size_t at = pgw_begin(w, 'B');
        pgw_u8(w, 0);
        pgw_str(w, stmt_name ? stmt_name : "");
        pgw_u16(w, 0);
        pgw_u16(w, (uint16_t)nparam);
        for (i = 0; i < nparam; i++) {
            if (pnull[i]) {
                pgw_u32(w, 0xffffffffu);
            } else {
                pgw_u32(w, (uint32_t)plen[i]);
                pgw_raw(w, pv[i], plen[i]);
            }
        }
        if (rfmt && nrfmt > 0) {
            int k;
            pgw_u16(w, (uint16_t)nrfmt);
            for (k = 0; k < nrfmt; k++)
                pgw_u16(w, rfmt[k] ? 1 : 0);
        } else {
            pgw_u16(w, 0);
        }
        pgw_end(w, at);
    }
    {
        size_t at = pgw_begin(w, 'D');
        pgw_u8(w, 'P');
        pgw_u8(w, 0);
        pgw_end(w, at);
    }
    {
        size_t at = pgw_begin(w, 'E');
        pgw_u8(w, 0);
        pgw_u32(w, 0);
        pgw_end(w, at);
    }
    return w->bad ? -1 : 0;
}

int pg_coerce_params(JSContext* ctx, JSValueConst params,
    const char*** out_pv, size_t** out_plen,
    uint8_t** out_pnull, uint8_t** out_pfree,
    uint32_t* out_n, const char* what)
{
    const char** pv = NULL;
    size_t* plen = NULL;
    uint8_t *pnull = NULL, *pfree = NULL;
    uint32_t nparam = 0, i;
    JSValue lv;

    *out_n = 0;
    if (!JS_IsArray(ctx, params)) {
        JS_ThrowTypeError(ctx, "%s: parameters must be an array", what);
        return -1;
    }
    lv = JS_GetPropertyStr(ctx, params, "length");
    if (JS_ToUint32(ctx, &nparam, lv) < 0) {
        JS_FreeValue(ctx, lv);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    if (nparam > 65535) {
        JS_ThrowRangeError(ctx, "%s: at most 65535 parameters", what);
        return -1;
    }
    if (nparam == 0)
        return 0;
    pv = (const char**)calloc(1, (size_t)nparam * (sizeof(*pv) + sizeof(*plen)) + (size_t)nparam * 2);
    if (!pv) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    plen = (size_t*)(pv + nparam);
    pnull = (uint8_t*)(plen + nparam);
    pfree = pnull + nparam;
    for (i = 0; i < nparam; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, params, i);
        if (JS_IsException(e))
            goto fail;
        if (JS_IsUndefined(e) || JS_IsNull(e)) {
            pnull[i] = 1;
            JS_FreeValue(ctx, e);
            continue;
        }
        if (JS_IsObject(e)) {
            int enc = pg_param_bytea(ctx, e, &pv[i], &plen[i]);
            if (enc > 0) {
                pfree[i] = PG_PF_MALLOC;
                JS_FreeValue(ctx, e);
                continue;
            }
            if (enc < 0) {
                JS_FreeValue(ctx, e);
                goto fail;
            }
            JS_FreeValue(ctx, e);
            pg_free_params(ctx, pv, pfree, i);
            pg_param_arrays_free(pv, plen, pnull, pfree);
            JS_ThrowTypeError(ctx,
                "%s: parameter %u is an object; a parameter is a value. "
                "Pass a Uint8Array or ArrayBuffer for bytea, "
                "JSON.stringify(v) for json/jsonb, "
                "or an ISO string for a timestamp",
                what, (unsigned)i + 1);
            return -1;
        }
        pv[i] = JS_ToCStringLen(ctx, &plen[i], e);
        pfree[i] = PG_PF_CSTR;
        JS_FreeValue(ctx, e);
        if (!pv[i])
            goto fail;
    }
    *out_pv = pv;
    *out_plen = plen;
    *out_pnull = pnull;
    *out_pfree = pfree;
    *out_n = nparam;
    return 0;

fail:
    pg_free_params(ctx, pv, pfree, i);
    pg_param_arrays_free(pv, plen, pnull, pfree);
    return -1;
}

int pg_encode_member(dyn_pg_t* g, pgw_t* w, const char* sql,
    int nparam, const char** pv, const size_t* plen,
    const uint8_t* pnull, char* name_out,
    size_t name_outcap)
{
    struct pg_stmt* stmt = NULL;
    const char* stmt_name = NULL;
    int stmt_prepared = 0;
    uint8_t* rfmt = NULL;
    int nrfmt = 0;

    stmt = pg_stmt_get(g, sql, strlen(sql));
    if (stmt && stmt->uses >= g->prepare_after) {
        stmt_name = stmt->name;
        stmt_prepared = stmt->prepared;
        if (g->binary_results && stmt->oids && stmt->noids > 0) {
            rfmt = (uint8_t*)malloc((size_t)stmt->noids);
            if (rfmt) {
                int k;
                nrfmt = stmt->noids;
                for (k = 0; k < nrfmt; k++)
                    rfmt[k] = (uint8_t)pg_oid_prefers_binary(g, stmt->oids[k]);
            }
        }
        stmt->prepared = 1;
        g->n_prepared_hits++;
    } else {
        g->n_unnamed++;
    }

    if (name_out && stmt_name)
        snprintf(name_out, name_outcap, "%s", stmt_name);
    {
        int rc = pg_encode_query(w, sql, nparam, pv, plen, pnull,
            stmt_name, stmt_prepared, rfmt, nrfmt);
        free(rfmt);
        if (rc < 0 && stmt_name && !stmt_prepared)
            stmt->prepared = 0;
        return rc;
    }
}

JSValue dyn_pg_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_pg_t* g;
    const char* sql = NULL;
    const char** pv = NULL;
    size_t* plen = NULL;
    uint8_t *pnull = NULL, *pfree = NULL;
    uint32_t nparam = 0;
    int have_params = 0;
    char pending_name[24];
    pgw_t w;
    dyn_pg_pending_t* p;
    JSValue funcs[2], promise;
    int64_t max_rows_opt = 0;
    int gates;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "query: a statement is required");

    if (argc > 2 && dyn_opts_strict(ctx, argv[2], pg_query_keys, 1))
        return JS_EXCEPTION;

    {
        size_t sql_len = 0;
        sql = JS_ToCStringLen(ctx, &sql_len, argv[0]);
        if (!sql)
            return JS_EXCEPTION;
        if (strlen(sql) != sql_len) {
            JS_FreeCString(ctx, sql);
            return JS_ThrowTypeError(ctx,
                "PostgreSQL: the SQL text contains U+0000; the protocol would cut it there");
        }
        gates = pg_sql_is_copy(sql);
    }
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        if (pg_coerce_params(ctx, argv[1], &pv, &plen, &pnull, &pfree,
                &nparam, "query")
            < 0) {
            JS_FreeCString(ctx, sql);
            return JS_EXCEPTION;
        }
        have_params = 1;
    }

    {
        int64_t mr = 0;
        if (argc > 2 && JS_IsObject(argv[2])) {
            JSValue mv = JS_GetPropertyStr(ctx, argv[2], "maxRows");
            if (JS_IsException(mv)) {
                JS_FreeValue(ctx, mv);
                JS_FreeCString(ctx, sql);
                return JS_EXCEPTION;
            }
            if (!JS_IsUndefined(mv) && !JS_IsNull(mv)) {
                if (JS_ToInt64(ctx, &mr, mv) || mr < 0) {
                    JS_FreeValue(ctx, mv);
                    JS_FreeCString(ctx, sql);
                    return JS_ThrowRangeError(ctx,
                        "PostgreSQL: maxRows must be >= 0");
                }
            }
            JS_FreeValue(ctx, mv);
        }
        max_rows_opt = mr;
    }

    g = pg_this(ctx, this_val);
    if (!g || g->state == PG_ST_DEAD) {
        pg_free_params(ctx, pv, pfree, nparam);
        pg_param_arrays_free(pv, plen, pnull, pfree);
        JS_FreeCString(ctx, sql);
        return g ? (g->errs ? JS_Throw(ctx, pg_conn_error(ctx, g->errs)) : JS_ThrowInternalError(ctx, "PostgreSQL: the connection is closed"))
                 : JS_EXCEPTION;
    }
    if (g->npending + g->nwait >= g->maxpending) {
        pg_free_params(ctx, pv, pfree, nparam);
        pg_param_arrays_free(pv, plen, pnull, pfree);
        JS_FreeCString(ctx, sql);
        return JS_ThrowInternalError(ctx,
            "PostgreSQL: %d queries already in flight (maxPending)", g->maxpending);
    }

    memset(&w, 0, sizeof(w));
    if (have_params) {
        char name[24];
        name[0] = '\0';
        if (pg_encode_member(g, &w, sql, (int)nparam, pv, plen, pnull,
                name, sizeof(name))
            < 0) {
            pg_free_params(ctx, pv, pfree, nparam);
            pg_param_arrays_free(pv, plen, pnull, pfree);
            JS_FreeCString(ctx, sql);
            return JS_ThrowOutOfMemory(ctx);
        }
        {
            size_t at = pgw_begin(&w, 'S');
            pgw_end(&w, at);
        }
        snprintf(pending_name, sizeof(pending_name), "%s", name);
    } else {
        size_t at = pgw_begin(&w, 'Q');
        pgw_str(&w, sql);
        pgw_end(&w, at);
        {
            size_t at2 = pgw_begin(&w, 'S');
            pgw_end(&w, at2);
        }
        pending_name[0] = '\0';
    }
    pg_free_params(ctx, pv, pfree, nparam);
    pg_param_arrays_free(pv, plen, pnull, pfree);
    JS_FreeCString(ctx, sql);

    p = pgp_new();
    if (!p) {
        free(w.b);
        return JS_ThrowOutOfMemory(ctx);
    }
    p->max_rows = max_rows_opt;
    snprintf(p->stmt_name, sizeof(p->stmt_name), "%s", pending_name);
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        free(w.b);
        pgp_free(ctx, p);
        return promise;
    }
    p->resolve = funcs[0];
    p->reject = funcs[1];

    p->gates = gates;
    if (pg_submit(g, p, w.b, w.len, ++g->sync_seq, 1) < 0) {
        free(p->bytes);
        p->bytes = NULL;
        pgp_free(ctx, p);
        JS_FreeValue(ctx, promise);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (g->state == PG_ST_READY)
        pg_flush_soon(g);
    pg_hold(ctx, this_val, g);
    return promise;
}
#endif
