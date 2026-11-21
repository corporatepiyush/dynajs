#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

#define DFP_DEFAULT_N 5u

JSValue dyn_df_nlargest(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const dfo_top_name[2] = { "N_LARGEST", "N_SMALLEST" };
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    DfoItem* it;
    double *dst, kd = 0;
    uint32_t n, m, k, i;

    if (dfo_open(ctx, this_val, argc, argv, 1, dfo_top_name[magic],
            &df, &b, &kd, &mask, NULL))
        return JS_EXCEPTION;
    if (isnan(kd) || isinf(kd) || kd < 0.0 || kd != floor(kd))
        return JS_ThrowRangeError(ctx, "%s(col, k): k must be a non-negative "
                                       "integer, got %g",
            dfo_top_name[magic], kd);
    if (dfo_gathered(ctx, &b, mask, df->nrows, &it, &n, &m))
        return JS_EXCEPTION;
    k = kd >= (double)m ? m : (uint32_t)kd;
    dfo_top(it, n, k, magic == DFO_TOP_LARGEST);
    dst = df_out_alloc(ctx, k, sizeof(double));
    if (!dst) {
        free(it);
        return JS_EXCEPTION;
    }
    for (i = 0; i < k; i++)
        dst[i] = it[i].key;
    free(it);
    return df_to_typed_array(ctx, dst, (size_t)k * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

JSValue dyn_df_nunique(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DfcSet set;
    const uint8_t* mask;
    int idx, ok;
    uint32_t n;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    memset(&set, 0, sizeof(set));
    if (dfc_scan(ctx, df, idx, mask, &set)) {
        dfc_set_free(&set);
        return JS_EXCEPTION;
    }
    n = set.nent;
    dfc_set_free(&set);
    return JS_NewInt64(ctx, n);
}

JSValue dyn_df_value_counts(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DfcSet set;
    const uint8_t* mask;
    uint32_t* ord = NULL;
    double* cnt = NULL;
    int idx, ok, mask_arg = (magic == DFC_VC_TOPK) ? 2 : 1;
    int32_t k = 0;
    uint32_t i, nout;
    JSValue keys, values;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (magic == DFC_VC_TOPK) {
        if (argc < 2 || JS_IsUndefined(argv[1]))
            return JS_ThrowTypeError(ctx, "TOP_K(col, k): k is required");
        if (JS_ToInt32(ctx, &k, argv[1]))
            return JS_EXCEPTION;
        if (k < 0)
            return JS_ThrowRangeError(ctx, "TOP_K: k must not be negative");
    }
    mask = df_mask_arg(ctx, argc > mask_arg ? argv[mask_arg] : JS_UNDEFINED,
        df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    memset(&set, 0, sizeof(set));
    if (dfc_scan(ctx, df, idx, mask, &set))
        goto fail;
    ord = dfc_rank_order(ctx, &set);
    if (!ord)
        goto fail;
    nout = set.nent;
    if (magic == DFC_VC_TOPK && (uint32_t)k < nout)
        nout = (uint32_t)k;
    cnt = df_out_alloc(ctx, nout, sizeof(double));
    if (!cnt)
        goto fail;
    for (i = 0; i < nout; i++)
        cnt[i] = set.counts[ord[i]];

    keys = dfc_keys_array(ctx, &df->cols[idx], &set, ord, nout);
    values = df_to_typed_array(ctx, cnt, (size_t)nout * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
    free(ord);
    dfc_set_free(&set);
    return dfc_pair(ctx, keys, values);

fail:
    free(cnt);
    free(ord);
    dfc_set_free(&set);
    return JS_EXCEPTION;
}

JSValue dyn_df_mode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DfcSet set;
    const uint8_t* mask;
    int idx, ok;
    uint32_t i, best = 0, bestc = 0;
    JSValue out;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    memset(&set, 0, sizeof(set));
    if (dfc_scan(ctx, df, idx, mask, &set)) {
        dfc_set_free(&set);
        return JS_EXCEPTION;
    }
    for (i = 0; i < set.nent; i++)
        if (set.counts[i] > bestc) {
            bestc = set.counts[i];
            best = i;
        }
    out = set.nent ? dfc_key_js(ctx, &df->cols[idx], set.keys[best])
                   : JS_UNDEFINED;
    dfc_set_free(&set);
    return out;
}

static uint32_t dfp_selected(const uint8_t* mask, uint32_t n)
{
    uint32_t i, k = 0;
    if (!mask)
        return n;
    for (i = 0; i < n; i++)
        k += (mask[i] != 0);
    return k;
}

static uint32_t dfp_first_selected(const uint8_t* mask, uint32_t n)
{
    uint32_t i;
    if (!mask)
        return n ? 0 : n;
    for (i = 0; i < n; i++)
        if (mask[i])
            return i;
    return n;
}

static uint32_t dfp_last_selected(const uint8_t* mask, uint32_t n)
{
    uint32_t i = n;
    if (!mask)
        return n ? n - 1 : n;
    while (i > 0) {
        i--;
        if (mask[i])
            return i;
    }
    return n;
}

JSValue dyn_df_head(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    const char* op = (magic == DFP_TAIL) ? "TAIL" : "HEAD";
    double want_d = (double)DFP_DEFAULT_N;
    double* dst;
    JSValueConst nv;
    uint32_t nrows, n, want, avail, skip, take, w, i, seen;
    int idx, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    nv = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (!JS_IsUndefined(nv) && !JS_IsNull(nv) && JS_ToFloat64(ctx, &want_d, nv))
        return JS_EXCEPTION;
    if (!(want_d >= 0))
        return JS_ThrowRangeError(ctx, "%s(col, n): n must be a non-negative "
                                       "number, got %g",
            op, want_d);
    nrows = df->nrows;
    want = (want_d >= (double)nrows) ? nrows : (uint32_t)want_d;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    dst = malloc(want ? (size_t)want * sizeof(double) : 1);
    if (!dst)
        return JS_ThrowOutOfMemory(ctx);
    if (dfp_bind_num(ctx, df, idx, &b, op)) {
        free(dst);
        return JS_EXCEPTION;
    }
    n = dfp_span(nrows, b.n);
    avail = dfp_selected(mask, n);
    take = (want < avail) ? want : avail;
    skip = (magic == DFP_TAIL) ? avail - take : 0;

    w = 0;
    if (!mask) {
        for (i = skip; w < take; i++)
            dst[w++] = df_get(b.p, b.type, i);
    } else {
        seen = 0;
        for (i = 0; i < n && w < take; i++) {
            if (!mask[i])
                continue;
            if (seen++ < skip)
                continue;
            dst[w++] = df_get(b.p, b.type, i);
        }
    }
    return df_to_typed_array(ctx, dst, (size_t)take * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

JSValue dyn_df_first(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    const char* op = (magic == DFP_LAST) ? "LAST" : "FIRST";
    uint32_t n, i;
    int idx, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dfp_bind_num(ctx, df, idx, &b, op))
        return JS_EXCEPTION;
    n = dfp_span(df->nrows, b.n);
    i = (magic == DFP_LAST) ? dfp_last_selected(mask, n)
                            : dfp_first_selected(mask, n);
    if (i >= n)
        return JS_UNDEFINED;
    return JS_NewFloat64(ctx, df_get(b.p, b.type, i));
}

JSValue dyn_df_describe(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    DFMoments mo;
    const uint8_t* mask;
    JSValue res;
    int idx, ok;
    double var;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dyn_df_bind(ctx, df, idx, &b))
        return JS_EXCEPTION;
    if (b.type == DF_STR)
        return JS_ThrowTypeError(ctx, "cannot reduce a string column");

    dfm_moments(b.p, b.type, NULL, DF_F64, mask, b.n, &mo);
    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        return res;
    var = mo.n >= 2.0 ? mo.m2x / (mo.n - 1.0) : DYN_NAN;

    if (dfm_set(ctx, res, "count", JS_NewInt64(ctx, (int64_t)mo.n)) < 0 || dfm_set(ctx, res, "sum", JS_NewFloat64(ctx, mo.sx)) < 0 || dfm_set(ctx, res, "mean", JS_NewFloat64(ctx, mo.n >= 1.0 ? mo.mx : DYN_NAN)) < 0 || dfm_set(ctx, res, "min", mo.n >= 1.0 ? JS_NewFloat64(ctx, mo.lo) : JS_UNDEFINED) < 0 || dfm_set(ctx, res, "max", mo.n >= 1.0 ? JS_NewFloat64(ctx, mo.hi) : JS_UNDEFINED) < 0 || dfm_set(ctx, res, "variance", JS_NewFloat64(ctx, var)) < 0 || dfm_set(ctx, res, "stddev", JS_NewFloat64(ctx, sqrt(var))) < 0 || dfm_set(ctx, res, "skew", JS_NewFloat64(ctx, dfm_skew(&mo))) < 0 || dfm_set(ctx, res, "kurtosis", JS_NewFloat64(ctx, dfm_kurt(&mo))) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

JSValue dyn_df_dtypes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    JSValue res;
    uint32_t i;

    (void)argc;
    (void)argv;
    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        return res;
    for (i = 0; i < df->ncols; i++) {
        if (JS_DefinePropertyValueStr(ctx, res, df->cols[i].name,
                JS_NewString(ctx, df_type_short(df->cols[i].type)),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
    }
    return res;
}

JSValue dyn_df_schema(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    JSValue res, item;
    uint32_t i;

    (void)argc;
    (void)argv;
    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    res = JS_NewArray(ctx);
    if (JS_IsException(res))
        return res;
    for (i = 0; i < df->ncols; i++) {
        item = JS_NewObject(ctx);
        if (JS_IsException(item) || JS_DefinePropertyValueStr(ctx, item, "name", JS_NewString(ctx, df->cols[i].name), JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueStr(ctx, item, "type", JS_NewString(ctx, df_type_short(df->cols[i].type)), JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueUint32(ctx, res, i, item, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
    }
    return res;
}

JSValue dyn_df_info(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    JSValue res, dtypes, bytes, total;
    uint64_t sum = 0;
    uint32_t i;

    (void)argc;
    (void)argv;
    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        return res;
    dtypes = JS_NewObject(ctx);
    bytes = JS_NewObject(ctx);
    total = JS_NewFloat64(ctx, 0);
    if (JS_IsException(dtypes) || JS_IsException(bytes) || JS_IsException(total))
        goto fail;
    for (i = 0; i < df->ncols; i++) {
        uint64_t b = df_col_bytes(&df->cols[i]);
        sum += b;
        if (JS_DefinePropertyValueStr(ctx, dtypes, df->cols[i].name,
                JS_NewString(ctx, df_type_short(df->cols[i].type)),
                JS_PROP_C_W_E)
                < 0
            || JS_DefinePropertyValueStr(ctx, bytes, df->cols[i].name,
                   JS_NewFloat64(ctx, (double)b),
                   JS_PROP_C_W_E)
                < 0)
            goto fail;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "rows", JS_NewInt64(ctx, df->nrows),
            JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, res, "cols", JS_NewInt64(ctx, df->ncols),
               JS_PROP_C_W_E)
            < 0)
        goto fail;
    {
        JSValue owned = dtypes;
        dtypes = JS_UNDEFINED;
        if (JS_DefinePropertyValueStr(ctx, res, "dtypes", owned, JS_PROP_C_W_E) < 0)
            goto fail;
    }
    {
        JSValue owned = bytes;
        bytes = JS_UNDEFINED;
        if (JS_DefinePropertyValueStr(ctx, res, "bytes", owned, JS_PROP_C_W_E) < 0)
            goto fail;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "total_bytes",
            JS_NewFloat64(ctx, (double)sum), JS_PROP_C_W_E)
        < 0)
        goto fail;
    JS_FreeValue(ctx, total);
    return res;
fail:
    JS_FreeValue(ctx, dtypes);
    JS_FreeValue(ctx, bytes);
    JS_FreeValue(ctx, total);
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}

JSValue dyn_df_memory_usage(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    JSValue res, cols, total;
    uint64_t sum = 0;
    uint32_t i;

    (void)argc;
    (void)argv;
    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        return res;
    cols = JS_NewObject(ctx);
    if (JS_IsException(cols)) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    for (i = 0; i < df->ncols; i++) {
        uint64_t b = df_col_bytes(&df->cols[i]);
        sum += b;
        if (JS_DefinePropertyValueStr(ctx, cols, df->cols[i].name,
                JS_NewFloat64(ctx, (double)b),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, cols);
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
    }
    total = JS_NewFloat64(ctx, (double)sum);
    if (JS_DefinePropertyValueStr(ctx, res, "columns", cols, JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueStr(ctx, res, "total", total, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

#endif
