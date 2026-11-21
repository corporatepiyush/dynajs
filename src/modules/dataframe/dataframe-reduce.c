#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

#define DF_COMBINE_SUM(p, q) ((p) + (q))
#define DF_COMBINE_MIN(p, q) ((p) < (q) ? (p) : (q))
#define DF_COMBINE_MAX(p, q) ((p) > (q) ? (p) : (q))
#define DF_ACC__(kind, sfx) DF_ACC_##kind##_##sfx
#define DF_ACC(kind, sfx) DF_ACC__(kind, sfx)
#define DF_ID__(kind, sfx) DF_ID_##kind##_##sfx
#define DF_ID(kind, sfx) DF_ID__(kind, sfx)

#define DF_DEFINE_REDUCE(method, suffix, ctype, acctype, INIT, STEP, COMBINE) \
    DF_DEFINE_REDUCE_K(DF_ACCS_FLOAT, method, suffix, ctype, acctype, INIT,   \
        STEP, COMBINE)

#define DF_REDUCE_CASE(method, sfx, cty, tag)                         \
    case tag:                                                         \
        if (mask)                                                     \
            acc = (double)df_##method##_masked_##sfx((const cty*)b.p, \
                mask, b.n, &count);                                   \
        else {                                                        \
            acc = (double)df_##method##_##sfx((const cty*)b.p, b.n);  \
            count = b.n;                                              \
        }                                                             \
        break;

#define DFL_DEFINE_ISUM(sfx, cty, acct)                                          \
    static acct dfl_isum_##sfx(const cty* restrict x, const uint8_t* restrict m, \
        uint32_t n)                                                              \
    {                                                                            \
        acct s = 0;                                                              \
        uint32_t i;                                                              \
        if (m) {                                                                 \
            for (i = 0; i < n; i++) {                                            \
                acct v = (acct)x[i];                                             \
                s += m[i] ? v : (acct)0;                                         \
            }                                                                    \
        } else {                                                                 \
            for (i = 0; i < n; i++)                                              \
                s += (acct)x[i];                                                 \
        }                                                                        \
        return s;                                                                \
    }

typedef struct {
    double lo, hi;
} DfrRange;

#define DF_DEF_SUM_F(sfx, cty, tag)                                        \
    DF_DEFINE_REDUCE_KM(DF_PASTE(DF_NACC_SUM_, sfx), DF_NACC_MASKED_FLOAT, \
        sum, sfx, cty,                                                     \
        DF_ACC(SUM, sfx), DF_ID(SUM, sfx), DF_STEP_SUM,                    \
        DF_COMBINE_SUM)

#define DF_DEF_SUM_I(sfx, cty, tag)                                  \
    DF_DEFINE_REDUCE_K(DF_ACCS_INT, sum, sfx, cty, DF_ACC(SUM, sfx), \
        DF_ID(SUM, sfx), DF_STEP_SUM, DF_COMBINE_SUM)

DF_FLOAT_TYPES(DF_DEF_SUM_F)

DF_INT_TYPES(DF_DEF_SUM_I)

#undef DF_DEF_SUM_F
#undef DF_DEF_SUM_I

#define DF_DEF_MIN_F(sfx, cty, tag)                    \
    DF_DEFINE_REDUCE(min, sfx, cty, DF_ACC(MIN, sfx),  \
        DF_ID(MIN, sfx), DF_PASTE(DF_STEP_FMIN_, sfx), \
        DF_COMBINE_MIN)

#define DF_DEF_MIN_I(sfx, cty, tag)                                  \
    DF_DEFINE_REDUCE_K(DF_ACCS_INT, min, sfx, cty, DF_ACC(MIN, sfx), \
        DF_ID(MIN, sfx), DF_STEP_MIN, DF_COMBINE_MIN)

DF_FLOAT_TYPES(DF_DEF_MIN_F)

DF_INT_TYPES(DF_DEF_MIN_I)

#undef DF_DEF_MIN_F
#undef DF_DEF_MIN_I

#define DF_DEF_MAX_F(sfx, cty, tag)                    \
    DF_DEFINE_REDUCE(max, sfx, cty, DF_ACC(MAX, sfx),  \
        DF_ID(MAX, sfx), DF_PASTE(DF_STEP_FMAX_, sfx), \
        DF_COMBINE_MAX)

#define DF_DEF_MAX_I(sfx, cty, tag)                                  \
    DF_DEFINE_REDUCE_K(DF_ACCS_INT, max, sfx, cty, DF_ACC(MAX, sfx), \
        DF_ID(MAX, sfx), DF_STEP_MAX, DF_COMBINE_MAX)

DF_FLOAT_TYPES(DF_DEF_MAX_F)

DF_INT_TYPES(DF_DEF_MAX_I)

#undef DF_DEF_MAX_F
#undef DF_DEF_MAX_I

JSValue dyn_df_reduce(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    int idx, ok;
    uint32_t i, count = 0;
    double acc = 0;

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

    if (magic == DF_COUNT) {
        if (!mask)
            return JS_NewInt64(ctx, b.n);
        for (i = 0; i < b.n; i++)
            count += (mask[i] != 0);
        return JS_NewInt64(ctx, count);
    }

    switch (magic) {
    case DF_MIN:
        switch ((int)(b.type)) {
#define DF_CASE(sfx, cty, tag) DF_REDUCE_CASE(min, sfx, cty, tag)
            DF_NUMERIC_TYPES(DF_CASE)
#undef DF_CASE
        default:
            return JS_ThrowTypeError(ctx, "cannot reduce a string column");
        }
        break;
    case DF_MAX:
        switch ((int)(b.type)) {
#define DF_CASE(sfx, cty, tag) DF_REDUCE_CASE(max, sfx, cty, tag)
            DF_NUMERIC_TYPES(DF_CASE)
#undef DF_CASE
        default:
            return JS_ThrowTypeError(ctx, "cannot reduce a string column");
        }
        break;
    default:
        switch ((int)(b.type)) {
#define DF_CASE(sfx, cty, tag) DF_REDUCE_CASE(sum, sfx, cty, tag)
            DF_NUMERIC_TYPES(DF_CASE)
#undef DF_CASE
        default:
            return JS_ThrowTypeError(ctx, "cannot reduce a string column");
        }
        break;
    }

    switch (magic) {
    case DF_MIN:
    case DF_MAX:
        return count ? JS_NewFloat64(ctx, acc) : JS_UNDEFINED;
    case DF_MEAN:
        return count ? JS_NewFloat64(ctx, acc / count)
                     : JS_NewFloat64(ctx, DYN_NAN);
    default:
        return JS_NewFloat64(ctx, acc);
    }
}

JSValue dyn_df_argmin(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    const char* op = (magic == DFP_ARGMAX) ? "ARG_MAX" : "ARG_MIN";
    double best = 0, v, *wv = NULL, *own = NULL;
    uint32_t n, i, at = 0;
    int idx, ok, have = 0;
    JSValue res;

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
    if (n) {
        if (b.type == DF_F64) {
            wv = DYN_UNCONST(b.p);
        } else {
            wv = own = malloc((size_t)n * sizeof(double));
            if (wv)
                df_widen(&b, wv, n);
        }
    }

#define DFP_ARG_SCAN(BETTER)                              \
    do {                                                  \
        for (i = 0; i < n; i++) {                         \
            if (mask && !mask[i])                         \
                continue;                                 \
            v = wv ? wv[i] : df_get_call(b.p, b.type, i); \
            if (isnan(v))                                 \
                continue;                                 \
            if (!have) {                                  \
                have = 1;                                 \
                best = v;                                 \
                at = i;                                   \
            } else if (BETTER) {                          \
                best = v;                                 \
                at = i;                                   \
            }                                             \
        }                                                 \
    } while (0)

#define DFP_LANES(BETTER)                                  \
    do {                                                   \
        double m0 = best, m1 = best, m2 = best, m3 = best; \
        uint32_t p0 = at, p1 = at, p2 = at, p3 = at;       \
        for (; i + 3 < n; i += 4) {                        \
            if (wv[i] BETTER m0) {                         \
                m0 = wv[i];                                \
                p0 = i;                                    \
            }                                              \
            if (wv[i + 1] BETTER m1) {                     \
                m1 = wv[i + 1];                            \
                p1 = i + 1;                                \
            }                                              \
            if (wv[i + 2] BETTER m2) {                     \
                m2 = wv[i + 2];                            \
                p2 = i + 2;                                \
            }                                              \
            if (wv[i + 3] BETTER m3) {                     \
                m3 = wv[i + 3];                            \
                p3 = i + 3;                                \
            }                                              \
        }                                                  \
        for (; i < n; i++)                                 \
            if (wv[i] BETTER m0) {                         \
                m0 = wv[i];                                \
                p0 = i;                                    \
            }                                              \
        if (m1 BETTER m0 || (m1 == m0 && p1 < p0)) {       \
            m0 = m1;                                       \
            p0 = p1;                                       \
        }                                                  \
        if (m3 BETTER m2 || (m3 == m2 && p3 < p2)) {       \
            m2 = m3;                                       \
            p2 = p3;                                       \
        }                                                  \
        if (m2 BETTER m0 || (m2 == m0 && p2 < p0)) {       \
            m0 = m2;                                       \
            p0 = p2;                                       \
        }                                                  \
        best = m0;                                         \
        at = p0;                                           \
    } while (0)

    if (wv && !mask) {
        for (i = 0; i < n; i++)
            if (!isnan(wv[i])) {
                have = 1;
                best = wv[i];
                at = i++;
                break;
            }
        if (have) {
            if (magic == DFP_ARGMAX)
                DFP_LANES(>);
            else
                DFP_LANES(<);
        }
    } else if (magic == DFP_ARGMAX)
        DFP_ARG_SCAN(v > best);
    else
        DFP_ARG_SCAN(v < best);
#undef DFP_ARG_SCAN

    res = have ? JS_NewInt64(ctx, at) : JS_UNDEFINED;
    free(own);
    return res;
}

#define DFL_ISUM_S(sfx, cty, tag) DFL_DEFINE_ISUM(sfx, cty, int64_t)
#define DFL_ISUM_U(sfx, cty, tag) DFL_DEFINE_ISUM(sfx, cty, uint64_t)

DFL_SINT_TYPES(DFL_ISUM_S)

DFL_UINT_TYPES(DFL_ISUM_U)

#undef DFL_ISUM_S
#undef DFL_ISUM_U

static int dfl_i64_exact_as_double(int64_t s, double d)
{
    if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0))
        return 0;
    return (int64_t)d == s;
}

static int dfl_u64_exact_as_double(uint64_t s, double d)
{
    if (!(d >= 0.0 && d < 18446744073709551616.0))
        return 0;
    return (uint64_t)d == s;
}

JSValue dyn_df_sumChecked(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    int idx, ok, is_signed = 0;
    int64_t si = 0;
    uint64_t ui = 0;
    double d = 0;

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

    switch ((int)(b.type)) {
#define X(sfx, cty, tag)                                 \
    case tag:                                            \
        si = dfl_isum_##sfx((const cty*)b.p, mask, b.n); \
        is_signed = 1;                                   \
        break;
        DFL_SINT_TYPES(X)
#undef X
#define X(sfx, cty, tag)                                 \
    case tag:                                            \
        ui = dfl_isum_##sfx((const cty*)b.p, mask, b.n); \
        break;
        DFL_UINT_TYPES(X)
#undef X
    default:
        return JS_ThrowTypeError(ctx,
            "SUM_CHECKED: column '%s' is %s; a checked sum is defined only on "
            "integer columns (Int32/Uint32/Int16/Uint16/Int8/Uint8 Array) -- "
            "use sum() for a floating-point column",
            df->cols[idx].name, df_type_name(b.type));
    }

    if (is_signed) {
        d = (double)si;
        if (!dfl_i64_exact_as_double(si, d)) {
            char nb[32];
            df_fmt_double(nb, sizeof nb, d);
            return JS_ThrowRangeError(ctx,
                "SUM_CHECKED: column '%s' totals %lld, which a Number cannot "
                "hold exactly; sum() returns %s",
                df->cols[idx].name, (long long)si, nb);
        }
    } else {
        d = (double)ui;
        if (!dfl_u64_exact_as_double(ui, d)) {
            char nb[32];
            df_fmt_double(nb, sizeof nb, d);
            return JS_ThrowRangeError(ctx,
                "SUM_CHECKED: column '%s' totals %llu, which a Number cannot "
                "hold exactly; sum() returns %s",
                df->cols[idx].name, (unsigned long long)ui, nb);
        }
    }
    return JS_NewFloat64(ctx, d);
}

static JSValue dfr_pair(JSContext* ctx, const char* na, JSValue a,
    const char* nb, JSValue b)
{
    JSValue res;

    if (JS_IsException(a) || JS_IsException(b)) {
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        return JS_EXCEPTION;
    }
    res = JS_NewObject(ctx);

    if (JS_IsException(res)) {
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, na, a, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, b);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, nb, b, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

static int dfr_cmp(const void* pa, const void* pb)
{
    double a = ((const DfrRange*)pa)->lo, b = ((const DfrRange*)pb)->lo;

    return a < b ? -1 : (a > b ? 1 : 0);
}

JSValue dyn_df_range_agg(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* op = magic == DFR_INTERSECT ? "RANGE_INTERSECT_AGG"
                                            : "RANGE_AGG";
    DataFrame* df;
    DFBound ba, bb;
    const uint8_t* mask;
    DfrRange* r = NULL;
    double *ls, *hs;
    uint32_t i, span, n = 0, out = 0;
    int ia, ib, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ia = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ia < 0)
        return JS_EXCEPTION;
    ib = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (ib < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, ia, &ba, op) || df_bind_numeric(ctx, df, ib, &bb, op))
        return JS_EXCEPTION;

    span = ba.n < bb.n ? ba.n : bb.n;
    if (span > df->nrows)
        span = df->nrows;
    r = df_out_alloc(ctx, span ? span : 1, sizeof(*r));
    if (!r)
        return JS_EXCEPTION;
    for (i = 0; i < span; i++) {
        double lo, hi;
        if (mask && !mask[i])
            continue;
        lo = df_get(ba.p, ba.type, i);
        hi = df_get(bb.p, bb.type, i);
        if (lo != lo || hi != hi || !(lo < hi))
            continue;
        if (magic != DFR_INTERSECT) {
            lo = lo == 0.0 ? 0.0 : lo;
            hi = hi == 0.0 ? 0.0 : hi;
        }
        r[n].lo = lo;
        r[n].hi = hi;
        n++;
    }

    if (magic == DFR_INTERSECT) {
        double lo, hi;
        if (n == 0) {
            free(r);
            return JS_UNDEFINED;
        }
        lo = r[0].lo;
        hi = r[0].hi;
        for (i = 1; i < n; i++) {
            if (r[i].lo > lo)
                lo = r[i].lo;
            if (r[i].hi < hi)
                hi = r[i].hi;
        }
        free(r);
        if (!(lo < hi))
            return JS_UNDEFINED;
        return dfr_pair(ctx, "start", JS_NewFloat64(ctx, lo),
            "end", JS_NewFloat64(ctx, hi));
    }

    qsort(r, n, sizeof(*r), dfr_cmp);
    for (i = 0; i < n; i++) {
        if (out > 0 && r[i].lo <= r[out - 1].hi) {
            if (r[i].hi > r[out - 1].hi)
                r[out - 1].hi = r[i].hi;
            continue;
        }
        r[out++] = r[i];
    }
    ls = df_out_alloc(ctx, out ? out : 1, sizeof(double));
    hs = df_out_alloc(ctx, out ? out : 1, sizeof(double));
    if (!ls || !hs) {
        free(r);
        free(ls);
        free(hs);
        return JS_EXCEPTION;
    }
    for (i = 0; i < out; i++) {
        ls[i] = r[i].lo;
        hs[i] = r[i].hi;
    }
    free(r);
    return dfr_pair(ctx, "starts",
        df_to_typed_array(ctx, ls, (size_t)out * sizeof(double),
            JS_TYPED_ARRAY_FLOAT64),
        "ends",
        df_to_typed_array(ctx, hs, (size_t)out * sizeof(double),
            JS_TYPED_ARRAY_FLOAT64));
}

#endif
