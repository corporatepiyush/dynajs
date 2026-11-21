#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

int dfo_sorted(JSContext* ctx, const DFBound* b, const uint8_t* mask,
    uint32_t nrows, int desc, DfoItem** out, uint32_t* pn)
{
    uint32_t span = b->n < nrows ? b->n : nrows, n;
    DfoItem* it = df_out_alloc(ctx, span, sizeof(DfoItem));

    if (!it)
        return -1;
    n = dfo_gather(b->p, b->type, mask, span, it);
    dfo_sort_any(it, n, desc);
    *out = it;
    *pn = n;
    return 0;
}

int dfo_gathered(JSContext* ctx, const DFBound* b, const uint8_t* mask,
    uint32_t nrows, DfoItem** out, uint32_t* pn,
    uint32_t* pvalued)
{
    uint32_t span = b->n < nrows ? b->n : nrows, n, i, m = 0;
    DfoItem* it = df_out_alloc(ctx, span, sizeof(DfoItem));

    if (!it)
        return -1;
    n = dfo_gather(b->p, b->type, mask, span, it);
    for (i = 0; i < n; i++)
        m += it[i].key == it[i].key;
    *out = it;
    *pn = n;
    *pvalued = m;
    return 0;
}

int dfo_open(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int nscalar, const char* op,
    DataFrame** pdf, DFBound* b, double* pscalar,
    const uint8_t** pmask, int* pout_idx)
{
    DataFrame* df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    int idx, ok, slot;
    JSValueConst maskv = JS_UNDEFINED;

    if (!df)
        return -1;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return -1;
    if (pout_idx)
        *pout_idx = -1;
    if (nscalar && argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]) && pout_idx && df_is_out_bag(ctx, argv[1])) {
        if (pscalar)
            *pscalar = 0.0;
        if (df_out_opt(ctx, df, argv[1], op, pout_idx) < 0)
            return -1;
    } else if (nscalar && JS_ToFloat64(ctx, pscalar, argc > 1 ? argv[1] : JS_UNDEFINED))
        return -1;
    for (slot = 1 + nscalar; slot < argc; slot++) {
        JSValueConst v = argv[slot];
        if (JS_IsUndefined(v) || JS_IsNull(v))
            continue;
        if (pout_idx && *pout_idx < 0 && df_is_out_bag(ctx, v)) {
            if (df_out_opt(ctx, df, v, op, pout_idx) < 0)
                return -1;
            continue;
        }
        if (JS_IsUndefined(maskv))
            maskv = v;
    }
    *pmask = df_mask_arg(ctx, maskv, df->nrows, &ok);
    if (!ok)
        return -1;
    if (df_bind_numeric(ctx, df, idx, b, op))
        return -1;
    *pdf = df;
    return 0;
}

JSValue dyn_df_sort(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const dfo_sort_name[2] = { "SORT", "ARG_SORT" };
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    DfoItem* it;
    uint32_t n, i;

    if (dfo_open(ctx, this_val, argc, argv, 0, dfo_sort_name[magic],
            &df, &b, NULL, &mask, NULL))
        return JS_EXCEPTION;
    if (dfo_sorted(ctx, &b, mask, df->nrows, 0, &it, &n))
        return JS_EXCEPTION;

    if (magic == DFO_ARGSORT) {
        uint32_t* dst = df_out_alloc(ctx, n, sizeof(uint32_t));
        if (!dst) {
            free(it);
            return JS_EXCEPTION;
        }
        for (i = 0; i < n; i++)
            dst[i] = it[i].idx;
        free(it);
        return df_to_typed_array(ctx, dst, (size_t)n * sizeof(uint32_t),
            JS_TYPED_ARRAY_UINT32);
    } else {
        double* dst = df_out_alloc(ctx, n, sizeof(double));
        if (!dst) {
            free(it);
            return JS_EXCEPTION;
        }
        for (i = 0; i < n; i++)
            dst[i] = it[i].key;
        free(it);
        return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
            JS_TYPED_ARRAY_FLOAT64);
    }
}

#endif
