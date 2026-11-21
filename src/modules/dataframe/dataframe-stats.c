#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

#define DF_COMBINE_MUL(p, q) ((p) * (q))

#define DF_DEFINE_VARIANCE(suffix, ctype, tag)                                                                                                                  \
    static double df_variance_##suffix(const ctype* restrict x,                                                                                                 \
        const uint8_t* restrict m, uint32_t n)                                                                                                                  \
    {                                                                                                                                                           \
        double s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0;                                                                                  \
        double q0 = 0, q1 = 0, q2 = 0, q3 = 0, q4 = 0, q5 = 0, q6 = 0, q7 = 0;                                                                                  \
        double mu, d0, d1, d2, d3, d4, d5, d6, d7, sd;                                                                                                          \
        double r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0;                                                                                  \
        uint32_t i = 0, k = 0;                                                                                                                                  \
        if (m) {                                                                                                                                                \
            for (; i + 8 <= n; i += 8) {                                                                                                                        \
                s0 += m[i] ? (double)x[i] : 0.0;                                                                                                                \
                s1 += m[i + 1] ? (double)x[i + 1] : 0.0;                                                                                                        \
                s2 += m[i + 2] ? (double)x[i + 2] : 0.0;                                                                                                        \
                s3 += m[i + 3] ? (double)x[i + 3] : 0.0;                                                                                                        \
                s4 += m[i + 4] ? (double)x[i + 4] : 0.0;                                                                                                        \
                s5 += m[i + 5] ? (double)x[i + 5] : 0.0;                                                                                                        \
                s6 += m[i + 6] ? (double)x[i + 6] : 0.0;                                                                                                        \
                s7 += m[i + 7] ? (double)x[i + 7] : 0.0;                                                                                                        \
                k += (m[i] != 0) + (m[i + 1] != 0) + (m[i + 2] != 0) + (m[i + 3] != 0) + (m[i + 4] != 0) + (m[i + 5] != 0) + (m[i + 6] != 0) + (m[i + 7] != 0); \
            }                                                                                                                                                   \
            for (; i < n; i++) {                                                                                                                                \
                s0 += m[i] ? (double)x[i] : 0.0;                                                                                                                \
                k += (m[i] != 0);                                                                                                                               \
            }                                                                                                                                                   \
        } else {                                                                                                                                                \
            for (; i + 8 <= n; i += 8) {                                                                                                                        \
                s0 += (double)x[i];                                                                                                                             \
                s1 += (double)x[i + 1];                                                                                                                         \
                s2 += (double)x[i + 2];                                                                                                                         \
                s3 += (double)x[i + 3];                                                                                                                         \
                s4 += (double)x[i + 4];                                                                                                                         \
                s5 += (double)x[i + 5];                                                                                                                         \
                s6 += (double)x[i + 6];                                                                                                                         \
                s7 += (double)x[i + 7];                                                                                                                         \
            }                                                                                                                                                   \
            for (; i < n; i++)                                                                                                                                  \
                s0 += (double)x[i];                                                                                                                             \
            k = n;                                                                                                                                              \
        }                                                                                                                                                       \
        if (k < 2)                                                                                                                                              \
            return DYN_NAN;                                                                                                                                     \
        mu = (((s0 + s1) + (s2 + s3)) + ((s4 + s5) + (s6 + s7))) / (double)k;                                                                                   \
        i = 0;                                                                                                                                                  \
        if (m) {                                                                                                                                                \
            for (; i + 8 <= n; i += 8) {                                                                                                                        \
                d0 = m[i] ? (double)x[i] - mu : 0.0;                                                                                                            \
                d1 = m[i + 1] ? (double)x[i + 1] - mu : 0.0;                                                                                                    \
                d2 = m[i + 2] ? (double)x[i + 2] - mu : 0.0;                                                                                                    \
                d3 = m[i + 3] ? (double)x[i + 3] - mu : 0.0;                                                                                                    \
                d4 = m[i + 4] ? (double)x[i + 4] - mu : 0.0;                                                                                                    \
                d5 = m[i + 5] ? (double)x[i + 5] - mu : 0.0;                                                                                                    \
                d6 = m[i + 6] ? (double)x[i + 6] - mu : 0.0;                                                                                                    \
                d7 = m[i + 7] ? (double)x[i + 7] - mu : 0.0;                                                                                                    \
                r0 += d0;                                                                                                                                       \
                q0 += d0 * d0;                                                                                                                                  \
                r1 += d1;                                                                                                                                       \
                q1 += d1 * d1;                                                                                                                                  \
                r2 += d2;                                                                                                                                       \
                q2 += d2 * d2;                                                                                                                                  \
                r3 += d3;                                                                                                                                       \
                q3 += d3 * d3;                                                                                                                                  \
                r4 += d4;                                                                                                                                       \
                q4 += d4 * d4;                                                                                                                                  \
                r5 += d5;                                                                                                                                       \
                q5 += d5 * d5;                                                                                                                                  \
                r6 += d6;                                                                                                                                       \
                q6 += d6 * d6;                                                                                                                                  \
                r7 += d7;                                                                                                                                       \
                q7 += d7 * d7;                                                                                                                                  \
            }                                                                                                                                                   \
            for (; i < n; i++) {                                                                                                                                \
                d0 = m[i] ? (double)x[i] - mu : 0.0;                                                                                                            \
                r0 += d0;                                                                                                                                       \
                q0 += d0 * d0;                                                                                                                                  \
            }                                                                                                                                                   \
        } else {                                                                                                                                                \
            for (; i + 8 <= n; i += 8) {                                                                                                                        \
                d0 = (double)x[i] - mu;                                                                                                                         \
                d1 = (double)x[i + 1] - mu;                                                                                                                     \
                d2 = (double)x[i + 2] - mu;                                                                                                                     \
                d3 = (double)x[i + 3] - mu;                                                                                                                     \
                d4 = (double)x[i + 4] - mu;                                                                                                                     \
                d5 = (double)x[i + 5] - mu;                                                                                                                     \
                d6 = (double)x[i + 6] - mu;                                                                                                                     \
                d7 = (double)x[i + 7] - mu;                                                                                                                     \
                r0 += d0;                                                                                                                                       \
                q0 += d0 * d0;                                                                                                                                  \
                r1 += d1;                                                                                                                                       \
                q1 += d1 * d1;                                                                                                                                  \
                r2 += d2;                                                                                                                                       \
                q2 += d2 * d2;                                                                                                                                  \
                r3 += d3;                                                                                                                                       \
                q3 += d3 * d3;                                                                                                                                  \
                r4 += d4;                                                                                                                                       \
                q4 += d4 * d4;                                                                                                                                  \
                r5 += d5;                                                                                                                                       \
                q5 += d5 * d5;                                                                                                                                  \
                r6 += d6;                                                                                                                                       \
                q6 += d6 * d6;                                                                                                                                  \
                r7 += d7;                                                                                                                                       \
                q7 += d7 * d7;                                                                                                                                  \
            }                                                                                                                                                   \
            for (; i < n; i++) {                                                                                                                                \
                d0 = (double)x[i] - mu;                                                                                                                         \
                r0 += d0;                                                                                                                                       \
                q0 += d0 * d0;                                                                                                                                  \
            }                                                                                                                                                   \
        }                                                                                                                                                       \
        sd = ((r0 + r1) + (r2 + r3)) + ((r4 + r5) + (r6 + r7));                                                                                                 \
        d0 = (((q0 + q1) + (q2 + q3)) + ((q4 + q5) + (q6 + q7))) - sd * (sd / (double)k);                                                                       \
        return (d0 < 0.0 ? 0.0 : d0) / (double)(k - 1);                                                                                                         \
    }

#define X(suffix, ctype, tag)                                \
    DF_DEFINE_REDUCE_KM(DF_ACCS_FLOAT, DF_NACC_MASKED_FLOAT, \
        product, suffix, ctype, double, 1.0,                 \
        DF_STEP_MUL, DF_COMBINE_MUL)

DF_NUMERIC_TYPES(X)

#undef X

DF_NUMERIC_TYPES(DF_DEFINE_VARIANCE)

JSValue dyn_df_product(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    int idx, ok;
    uint32_t count = 0;
    double acc;

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

#define X(suffix, ctype, tag)                                          \
    case tag:                                                          \
        acc = mask                                                     \
            ? df_product_masked_##suffix((const ctype*)b.p, mask, b.n, \
                  &count)                                              \
            : df_product_##suffix((const ctype*)b.p, b.n);             \
        break;
    switch ((int)(b.type)) {
        DF_NUMERIC_TYPES(X)
    default:
        return JS_ThrowTypeError(ctx, "cannot reduce a string column");
    }
#undef X
    (void)count;
    return JS_NewFloat64(ctx, acc);
}

JSValue dyn_df_variance(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    int idx, ok;
    double v;

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

#define X(suffix, ctype, tag)                                   \
    case tag:                                                   \
        v = df_variance_##suffix((const ctype*)b.p, mask, b.n); \
        break;
    switch ((int)(b.type)) {
        DF_NUMERIC_TYPES(X)
    default:
        return JS_ThrowTypeError(ctx, "cannot reduce a string column");
    }
#undef X
    return JS_NewFloat64(ctx, magic == DF_VAR_STDDEV ? sqrt(v) : v);
}

JSValue dyn_df_rank(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    DfoItem* it;
    double* dst;
    uint32_t nrows, n, m, i, j;

    if (dfo_open(ctx, this_val, argc, argv, 0, "RANK", &df, &b, NULL, &mask, NULL))
        return JS_EXCEPTION;
    nrows = df->nrows;
    if (dfo_sorted(ctx, &b, mask, nrows, 0, &it, &n))
        return JS_EXCEPTION;
    dst = df_out_alloc(ctx, nrows, sizeof(double));
    if (!dst) {
        free(it);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nrows; i++)
        dst[i] = DYN_NAN;

    m = dfo_valued(it, n);
    i = 0;
    while (i < m) {
        double r;
        for (j = i + 1; j < m && it[j].key == it[i].key; j++)
            ;
        r = ((double)(i + 1) + (double)j) * 0.5;
        for (; i < j; i++)
            dst[it[i].idx] = r;
    }
    free(it);
    return df_to_typed_array(ctx, dst, (size_t)nrows * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}


static double dfm_var_pop(const DFMoments* o)
{
    return o->n >= 1.0 ? o->m2x / o->n : DYN_NAN;
}

double dfm_skew(const DFMoments* o)
{
    double m2, m3;
    if (!(o->m2x > 0.0))
        return DYN_NAN;
    m2 = o->m2x / o->n;
    m3 = o->m3x / o->n;
    return m3 / (m2 * sqrt(m2));
}

double dfm_kurt(const DFMoments* o)
{
    double m2, m4;
    if (!(o->m2x > 0.0))
        return DYN_NAN;
    m2 = o->m2x / o->n;
    m4 = o->m4x / o->n;
    return m4 / (m2 * m2) - 3.0;
}

static double dfm_corr(const DFMoments* o)
{
    double r;
    if (!(o->m2x > 0.0) || !(o->m2y > 0.0))
        return DYN_NAN;
    r = o->cxy / (sqrt(o->m2x) * sqrt(o->m2y));
    if (r > 1.0)
        r = 1.0;
    if (r < -1.0)
        r = -1.0;
    return r;
}

JSValue dyn_df_moments1(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    DFMoments mo;
    const uint8_t* mask;
    int idx, ok;
    double r;

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
    switch (magic) {
    case DFM_STDPOP:
        r = sqrt(dfm_var_pop(&mo));
        break;
    case DFM_SKEW:
        r = dfm_skew(&mo);
        break;
    case DFM_KURT:
        r = dfm_kurt(&mo);
        break;
    default:
        r = dfm_var_pop(&mo);
        break;
    }
    return JS_NewFloat64(ctx, r);
}

JSValue dyn_df_moments2(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const names[] = {
        "COV_POP", "COV_SAMP", "CORR", "REGR_SLOPE", "REGR_INTERCEPT", "REGR_R2",
        "REGR_AVG_X", "REGR_AVG_Y"
    };
    DataFrame* df;
    DFBound ba, bb;
    const DFBound *bx, *by;
    DFMoments mo;
    const uint8_t* mask;
    int ia, ib, ok, swap;
    uint32_t n;
    double r, rr;

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

    if (dyn_df_bind(ctx, df, ia, &ba) || dyn_df_bind(ctx, df, ib, &bb))
        return JS_EXCEPTION;
    if (ba.type == DF_STR || bb.type == DF_STR) {
        int bad = ba.type == DF_STR ? ia : ib;
        return JS_ThrowTypeError(ctx, "%s: column '%s' is %s; cannot reduce a "
                                      "string column",
            names[magic],
            df->cols[bad].name, df_type_name(DF_STR));
    }

    switch (magic) {
    case DFM_SLOPE:
    case DFM_INTERCEPT:
    case DFM_R2:
    case DFM_AVGX:
    case DFM_AVGY:
        swap = 1;
        break;
    default:
        swap = 0;
        break;
    }
    bx = swap ? &bb : &ba;
    by = swap ? &ba : &bb;

    n = bx->n < by->n ? bx->n : by->n;
    dfm_moments(bx->p, bx->type, by->p, by->type, mask, n, &mo);

    switch (magic) {
    case DFM_COVSAMP:
        r = mo.n >= 2.0 ? mo.cxy / (mo.n - 1.0) : DYN_NAN;
        break;
    case DFM_CORR:
        r = dfm_corr(&mo);
        break;
    case DFM_SLOPE:
        r = mo.m2x > 0.0 ? mo.cxy / mo.m2x : DYN_NAN;
        break;
    case DFM_INTERCEPT:
        r = mo.m2x > 0.0 ? mo.my - (mo.cxy / mo.m2x) * mo.mx : DYN_NAN;
        break;
    case DFM_R2:
        rr = dfm_corr(&mo);
        r = rr * rr;
        if (r > 1.0)
            r = 1.0;
        break;
    case DFM_AVGX:
        r = mo.n >= 1.0 ? mo.mx : DYN_NAN;
        break;
    case DFM_AVGY:
        r = mo.n >= 1.0 ? mo.my : DYN_NAN;
        break;
    default:
        r = mo.n >= 1.0 ? mo.cxy / mo.n : DYN_NAN;
        break;
    }
    return JS_NewFloat64(ctx, r);
}

JSValue dyn_df_mean_weighted(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound bv, bw;
    const uint8_t* mask;
    int iv, iw, ok;
    uint32_t i, n;
    double sw = 0.0, swx = 0.0, v, w;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    iv = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (iv < 0)
        return JS_EXCEPTION;
    iw = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (iw < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dyn_df_bind(ctx, df, iv, &bv) || dyn_df_bind(ctx, df, iw, &bw))
        return JS_EXCEPTION;
    if (bv.type == DF_STR || bw.type == DF_STR) {
        int bad = bv.type == DF_STR ? iv : iw;
        return JS_ThrowTypeError(ctx, "MEAN_WEIGHTED: column '%s' is %s; cannot "
                                      "reduce a string column",
            df->cols[bad].name,
            df_type_name(DF_STR));
    }

    n = bv.n < bw.n ? bv.n : bw.n;
    for (i = 0; i < n; i++) {
        if (mask && !mask[i])
            continue;
        w = df_get(bw.p, bw.type, i);
        if (w == 0.0)
            continue;
        v = df_get(bv.p, bv.type, i);
        sw += w;
        swx += w * v;
    }
    return JS_NewFloat64(ctx, sw == 0.0 ? DYN_NAN : swx / sw);
}

int dfm_set(JSContext* ctx, JSValue obj, const char* key, JSValue v)
{
    if (JS_IsException(v))
        return -1;
    return JS_DefinePropertyValueStr(ctx, obj, key, v, JS_PROP_C_W_E);
}

static double dfx_skew_samp(const DFMoments* o)
{
    double n = o->n, g1;

    if (n < 3.0 || !(o->m2x > 0.0))
        return 0.0;
    g1 = (o->m3x / n) / pow(o->m2x / n, 1.5);
    return g1 * sqrt(n * (n - 1.0)) / (n - 2.0);
}

static double dfx_kurt_samp(const DFMoments* o)
{
    double n = o->n, g2;

    if (n < 4.0 || !(o->m2x > 0.0))
        return 0.0;
    g2 = (o->m4x / n) / ((o->m2x / n) * (o->m2x / n)) - 3.0;
    return ((n + 1.0) * g2 + 6.0) * (n - 1.0) / ((n - 2.0) * (n - 3.0));
}

JSValue dyn_df_stat1(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const nm[4] = { "SEM", "SKEW_SAMP", "KURT_SAMP",
        "COUNT_NULLS" };
    DataFrame* df;
    DFBound b;
    DFMoments mo;
    const uint8_t* mask;
    int idx, ok;
    double r;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b, nm[magic]))
        return JS_EXCEPTION;

    if (magic == DFX_COUNT_NULLS) {
        uint32_t i, span = b.n < df->nrows ? b.n : df->nrows, c = 0;
        for (i = 0; i < span; i++) {
            double v;
            if (mask && !mask[i])
                continue;
            v = df_get(b.p, b.type, i);
            c += (v != v);
        }
        return JS_NewInt64(ctx, (int64_t)c);
    }

    dfm_moments(b.p, b.type, NULL, DF_F64, mask, b.n, &mo);
    switch (magic) {
    case DFX_SKEW_SAMP:
        r = dfx_skew_samp(&mo);
        break;
    case DFX_KURT_SAMP:
        r = dfx_kurt_samp(&mo);
        break;
    default:
        r = mo.n > 1.0 ? sqrt(mo.m2x / (mo.n - 1.0)) / sqrt(mo.n) : DYN_NAN;
        break;
    }
    return JS_NewFloat64(ctx, r);
}

JSValue dyn_df_regr_sum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const nm[4] = { "REGR_COUNT", "REGR_SXX", "REGR_SYY",
        "REGR_SXY" };
    DataFrame* df;
    DFBound bx, by;
    DFMoments mo;
    const uint8_t* mask;
    int ix, iy, ok;
    uint32_t n;
    double r;
    const char* op = nm[magic - DFX_REGR_COUNT];

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ix = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ix < 0)
        return JS_EXCEPTION;
    iy = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (iy < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, ix, &bx, op) || df_bind_numeric(ctx, df, iy, &by, op))
        return JS_EXCEPTION;

    n = bx.n < by.n ? bx.n : by.n;
    dfm_moments(bx.p, bx.type, by.p, by.type, mask, n, &mo);
    switch (magic) {
    case DFX_REGR_SXX:
        r = mo.m2y;
        break;
    case DFX_REGR_SYY:
        r = mo.m2x;
        break;
    case DFX_REGR_SXY:
        r = mo.cxy;
        break;
    default:
        r = mo.n;
        break;
    }
    return JS_NewFloat64(ctx, r);
}

JSValue dyn_df_deviation(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    DfoItem* it = NULL;
    uint32_t n, m, i;
    int idx, ok;
    double centre, r;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b,
            magic == DFX_MAD ? "MAD" : "MEDIAN_ABSOLUTE_DEVIATION"))
        return JS_EXCEPTION;
    if (dfo_gathered(ctx, &b, mask, df->nrows, &it, &n, &m))
        return JS_EXCEPTION;
    if (m == 0) {
        free(it);
        return JS_UNDEFINED;
    }

    if (magic == DFX_MAD) {
        double s = 0.0, comp = 0.0, lo;
        for (i = 0; i < n; i++) {
            double v = it[i].key, t;
            if (v != v)
                continue;
            t = s + v;
            comp += fabs(s) >= fabs(v) ? (s - t) + v : (v - t) + s;
            s = t;
        }
        centre = s / (double)m;
        lo = (fma(-centre, (double)m, s) + comp) / (double)m;
        s = 0.0;
        for (i = 0; i < n; i++)
            if (it[i].key == it[i].key)
                s += fabs((it[i].key - centre) - lo);
        r = s / (double)m;
    } else {
        dfo_select(it, n, m >> 1, 0);
        centre = it[m >> 1].key;
        if ((m & 1) == 0 && m >= 2) {
            dfo_select(it, n, (m >> 1) - 1, 0);
            centre = (centre + it[(m >> 1) - 1].key) * 0.5;
        }
        for (i = 0; i < n; i++)
            it[i].key = it[i].key == it[i].key ? fabs(it[i].key - centre) : DYN_NAN;
        dfo_select(it, n, m >> 1, 0);
        r = it[m >> 1].key;
        if ((m & 1) == 0 && m >= 2) {
            dfo_select(it, n, (m >> 1) - 1, 0);
            r = (r + it[(m >> 1) - 1].key) * 0.5;
        }
    }
    free(it);
    return JS_NewFloat64(ctx, r);
}

JSValue dyn_df_entropy(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DfcSet set;
    const uint8_t* mask;
    uint32_t i, total = 0;
    int idx, ok;
    double h = 0.0;

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
        total += set.counts[i];
    if (total) {
        double inv = 1.0 / (double)total;
        for (i = 0; i < set.nent; i++) {
            double pr = (double)set.counts[i] * inv;
            h -= pr * log2(pr);
        }
    }
    dfc_set_free(&set);
    return JS_NewFloat64(ctx, h);
}

JSValue dyn_df_corr_matrix(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    const char* op = magic ? "COV_MATRIX" : "CORR_MATRIX";
    const uint8_t* mask;
    JSValue arr = JS_UNDEFINED, lenv, cols = JS_UNDEFINED, mv, res;
    int *idx = NULL, ok;
    uint32_t* first = NULL;
    double* m = NULL;
    uint32_t nc = 0, i, j;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;

    if (argc > 0 && JS_IsObject(argv[0])) {
        arr = JS_DupValue(ctx, argv[0]);
        lenv = JS_GetPropertyStr(ctx, arr, "length");
        if (JS_IsException(lenv) || JS_ToUint32(ctx, &nc, lenv) < 0) {
            JS_FreeValue(ctx, lenv);
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lenv);
    } else {
        return JS_ThrowTypeError(ctx, "%s(cols): cols must be an array "
                                      "of column names",
            op);
    }
    if (nc == 0 || nc > DF_MAX_COLS) {
        JS_FreeValue(ctx, arr);
        return JS_ThrowRangeError(ctx, "%s: between 1 and %d columns", op,
            DF_MAX_COLS);
    }
    idx = df_out_alloc(ctx, nc, sizeof(int));
    if (!idx) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nc; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        idx[i] = JS_IsException(e) ? -1 : df_col_arg(ctx, df, e);
        JS_FreeValue(ctx, e);
        if (idx[i] < 0) {
            JS_FreeValue(ctx, arr);
            free(idx);
            return JS_EXCEPTION;
        }
    }
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok) {
        JS_FreeValue(ctx, arr);
        free(idx);
        return JS_EXCEPTION;
    }

    m = df_out_alloc(ctx, nc * nc, sizeof(double));
    first = df_out_alloc(ctx, nc, sizeof(uint32_t));
    if (!m || !first) {
        JS_FreeValue(ctx, arr);
        free(idx);
        free(m);
        free(first);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nc; i++) {
        first[i] = i;
        for (j = 0; j < i; j++)
            if (idx[j] == idx[i]) {
                first[i] = j;
                break;
            }
    }

    for (i = 0; i < nc; i++) {
        for (j = i; j < nc; j++) {
            if (first[i] != i || first[j] != j) {
                double dup = m[first[i] * nc + first[j]];
                m[i * nc + j] = dup;
                m[j * nc + i] = dup;
                continue;
            }
            DFBound bx, by;
            DFMoments mo;
            double r;
            uint32_t n;
            if (df_bind_numeric(ctx, df, idx[i], &bx, op) || df_bind_numeric(ctx, df, idx[j], &by, op)) {
                JS_FreeValue(ctx, arr);
                free(idx);
                free(m);
                free(first);
                return JS_EXCEPTION;
            }
            n = bx.n < by.n ? bx.n : by.n;
            dfm_moments(bx.p, bx.type, by.p, by.type, mask, n, &mo);
            r = magic      ? (mo.n >= 2.0 ? mo.cxy / (mo.n - 1.0) : DYN_NAN)
                : (i == j) ? (mo.m2x > 0.0 ? 1.0 : DYN_NAN)
                           : dfm_corr(&mo);
            m[i * nc + j] = r;
            m[j * nc + i] = r;
        }
    }

    cols = JS_NewArray(ctx);
    for (i = 0; i < nc && !JS_IsException(cols); i++) {
        JSValue s = JS_NewString(ctx, df->cols[idx[i]].name);
        if (JS_IsException(s) || JS_DefinePropertyValueUint32(ctx, cols, i, s, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, cols);
            cols = JS_EXCEPTION;
        }
    }
    JS_FreeValue(ctx, arr);
    free(idx);
    free(first);
    mv = df_to_typed_array(ctx, m, (size_t)nc * nc * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
    res = JS_NewObject(ctx);
    if (JS_IsException(cols) || JS_IsException(mv) || JS_IsException(res)) {
        JS_FreeValue(ctx, cols);
        JS_FreeValue(ctx, mv);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "columns", cols, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);
        JS_FreeValue(ctx, mv);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "matrix", mv, JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueStr(ctx, res, "n", JS_NewInt64(ctx, nc), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

JSValue dyn_df_zscore(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    DFMoments mo;
    const uint8_t* mask;
    double sd, *dst;
    uint32_t i, n, span;
    int out_idx = -1;

    if (dfo_open(ctx, this_val, argc, argv, 0, "ZSCORE", &df, &b, NULL, &mask, &out_idx))
        return JS_EXCEPTION;
    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    span = df_map_span(n, b.n, dst);
    dfm_moments(b.p, b.type, NULL, DF_F64, mask, b.n, &mo);
    sd = mo.n >= 2.0 ? sqrt(mo.m2x / (mo.n - 1.0)) : DYN_NAN;

    for (i = 0; i < span; i++) {
        if (mask && !mask[i]) {
            dst[i] = DYN_NAN;
            continue;
        }
        dst[i] = (df_get(b.p, b.type, i) - mo.mx) / sd;
    }
    if (out_idx >= 0) {
        if (df_store_out(ctx, df, out_idx, dst, n)) {
            free(dst);
            return JS_EXCEPTION;
        }
        free(dst);
        return JS_DupValue(ctx, this_val);
    }
    return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

JSValue dyn_df_rank_ext(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    DfoItem* it;
    double* dst;
    uint32_t nrows, n, m, i, j, dense = 0;
    const char* op = magic == DFX_PERCENT_RANK ? "PERCENT_RANK" : "DENSE_RANK";

    if (dfo_open(ctx, this_val, argc, argv, 0, op, &df, &b, NULL, &mask, NULL))
        return JS_EXCEPTION;
    nrows = df->nrows;
    if (dfo_sorted(ctx, &b, mask, nrows, 0, &it, &n))
        return JS_EXCEPTION;
    dst = df_out_alloc(ctx, nrows, sizeof(double));
    if (!dst) {
        free(it);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nrows; i++)
        dst[i] = DYN_NAN;

    m = dfo_valued(it, n);
    i = 0;
    while (i < m) {
        double r;
        for (j = i + 1; j < m && it[j].key == it[i].key; j++)
            ;
        dense++;
        r = magic == DFX_PERCENT_RANK
            ? (m > 1 ? (double)i / (double)(m - 1) : 0.0)
            : (double)dense;
        for (; i < j; i++)
            dst[it[i].idx] = r;
    }
    free(it);
    return df_to_typed_array(ctx, dst, (size_t)nrows * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

JSValue dyn_df_ntile(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    DfoItem* it;
    double kd, *dst;
    uint32_t nrows, n, m, i, k, big, small, cut;

    if (dfo_open(ctx, this_val, argc, argv, 1, "NTILE", &df, &b, &kd, &mask, NULL))
        return JS_EXCEPTION;
    if (!(kd >= 1) || kd != floor(kd) || kd > (double)UINT32_MAX)
        return JS_ThrowRangeError(ctx, "NTILE: buckets must be a positive "
                                       "integer, got %g",
            kd);
    k = (uint32_t)kd;
    nrows = df->nrows;
    if (dfo_sorted(ctx, &b, mask, nrows, 0, &it, &n))
        return JS_EXCEPTION;
    dst = df_out_alloc(ctx, nrows, sizeof(double));
    if (!dst) {
        free(it);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nrows; i++)
        dst[i] = DYN_NAN;

    m = dfo_valued(it, n);
    big = m % k;
    small = m / k;
    cut = big * (small + 1);
    for (i = 0; i < m; i++) {
        uint32_t t = i < cut ? i / (small + 1)
                             : big + (small ? (i - cut) / small : 0);
        dst[it[i].idx] = (double)(t + 1);
    }
    free(it);
    return df_to_typed_array(ctx, dst, (size_t)nrows * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

JSValue dyn_df_rank_corr(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound bx, by;
    DFMoments mo;
    const uint8_t* mask;
    uint8_t* both = NULL;
    DfoItem* it = NULL;
    double *rx = NULL, *ry = NULL, r;
    uint32_t nrows, span, n, m, i, j, pass;
    int ix, iy, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ix = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ix < 0)
        return JS_EXCEPTION;
    iy = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (iy < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, ix, &bx, "RANK_CORR") || df_bind_numeric(ctx, df, iy, &by, "RANK_CORR"))
        return JS_EXCEPTION;

    nrows = df->nrows;
    span = bx.n < by.n ? bx.n : by.n;
    both = df_out_alloc(ctx, nrows, sizeof(uint8_t));
    rx = df_out_alloc(ctx, nrows, sizeof(double));
    ry = df_out_alloc(ctx, nrows, sizeof(double));
    if (!both || !rx || !ry)
        goto oom;
    memset(both, 0, nrows);
    for (i = 0; i < span; i++) {
        double a = df_get(bx.p, bx.type, i), c = df_get(by.p, by.type, i);
        both[i] = (!mask || mask[i]) && a == a && c == c;
    }

    for (pass = 0; pass < 2; pass++) {
        const DFBound* bp = pass ? &by : &bx;
        double* out = pass ? ry : rx;
        if (dfo_sorted(ctx, bp, both, nrows, 0, &it, &n))
            goto oom;
        m = dfo_valued(it, n);
        i = 0;
        while (i < m) {
            double v;
            for (j = i + 1; j < m && it[j].key == it[i].key; j++)
                ;
            v = ((double)(i + 1) + (double)j) * 0.5;
            for (; i < j; i++)
                out[it[i].idx] = v;
        }
        free(it);
        it = NULL;
    }

    dfm_moments(rx, DF_F64, ry, DF_F64, both, span, &mo);
    r = dfm_corr(&mo);
    free(both);
    free(rx);
    free(ry);
    return JS_NewFloat64(ctx, r);
oom:
    free(it);
    free(both);
    free(rx);
    free(ry);
    return JS_EXCEPTION;
}

static const char* dfe_name(int magic)
{
    switch (magic) {
    case DFE_SUM:
        return "EXPONENTIAL_TIME_DECAYED_SUM";
    case DFE_COUNT:
        return "EXPONENTIAL_TIME_DECAYED_COUNT";
    case DFE_MAX:
        return "EXPONENTIAL_TIME_DECAYED_MAX";
    default:
        return "EXPONENTIAL_TIME_DECAYED_AVG";
    }
}

JSValue dyn_df_etd_avg(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound bv, bt;
    const uint8_t* mask;
    const char* op = dfe_name(magic);
    uint32_t i, span;
    int iv, it_, ok, have = 0, hit = 0;
    double tau = 0, tmax = 0, num = 0, den = 0, best = 0;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    iv = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (iv < 0)
        return JS_EXCEPTION;
    it_ = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (it_ < 0)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &tau, argc > 2 ? argv[2] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (!(tau > 0.0))
        return JS_ThrowRangeError(ctx, "%s(value, time, tau): tau must be "
                                       "positive, got %g",
            op, tau);
    mask = df_mask_arg(ctx, argc > 3 ? argv[3] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, iv, &bv, op) || df_bind_numeric(ctx, df, it_, &bt, op))
        return JS_EXCEPTION;

    span = bv.n < bt.n ? bv.n : bt.n;
    if (span > df->nrows)
        span = df->nrows;
    for (i = 0; i < span; i++) {
        double t;
        if (mask && !mask[i])
            continue;
        t = df_get(bt.p, bt.type, i);
        if (t != t)
            continue;
        if (!have || t > tmax)
            tmax = t;
        have = 1;
    }
    if (!have)
        return JS_UNDEFINED;
    for (i = 0; i < span; i++) {
        double v, t, w;
        if (mask && !mask[i])
            continue;
        v = df_get(bv.p, bv.type, i);
        t = df_get(bt.p, bt.type, i);
        if (v != v || t != t)
            continue;
        w = exp((t - tmax) / tau);
        num += v * w;
        den += w;
        if (!hit || v * w > best)
            best = v * w;
        hit = 1;
    }
    if (!hit)
        return JS_UNDEFINED;
    switch (magic) {
    case DFE_SUM:
        return JS_NewFloat64(ctx, num);
    case DFE_COUNT:
        return JS_NewFloat64(ctx, den);
    case DFE_MAX:
        return JS_NewFloat64(ctx, best);
    default:
        break;
    }
    return den > 0.0 ? JS_NewFloat64(ctx, num / den) : JS_UNDEFINED;
}

#endif
