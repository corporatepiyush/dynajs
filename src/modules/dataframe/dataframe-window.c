#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

#define DFG_ROLL(INIT, STEP, FINISH)                     \
    do {                                                 \
        for (i = 0; i < span; i++) {                     \
            double a = (INIT);                           \
            uint32_t c = 0, j;                           \
            if (i < w - 1) {                             \
                dst[i] = DYN_NAN;                        \
                continue;                                \
            }                                            \
            for (j = i - (w - 1); j <= i; j++) {         \
                double v;                                \
                if (mask && !mask[j])                    \
                    continue;                            \
                v = fx ? fx[j] : df_get(b.p, b.type, j); \
                STEP;                                    \
                c++;                                     \
            }                                            \
            dst[i] = (FINISH);                           \
        }                                                \
    } while (0)

#define DFG_ROLL_POLL_WORK 4194304u

static void dfs_widen(const DFBound* b, double* dst, uint32_t n)
{
    df_widen(b, dst, n);
}

#define DFS_SCAN(name, STEP, ID)                           \
    static void dfs_##name(double* v, uint32_t n)          \
    {                                                      \
        double acc = (ID);                                 \
        uint32_t i;                                        \
        for (i = 0; i < n; i++) {                          \
            double x = v[i];                               \
            STEP(acc, x);                                  \
            v[i] = acc;                                    \
        }                                                  \
    }                                                      \
    static void dfs_##name##_masked(double* v, uint32_t n, \
        const uint8_t* mask)                               \
    {                                                      \
        double acc = (ID);                                 \
        uint32_t i;                                        \
        for (i = 0; i < n; i++) {                          \
            double x = mask[i] ? v[i] : (ID);              \
            STEP(acc, x);                                  \
            v[i] = acc;                                    \
        }                                                  \
    }

DFS_SCAN(cumsum, DF_STEP_SUM, 0.0)

DFS_SCAN(cumprod, DF_STEP_MUL, 1.0)

DFS_SCAN(cummax, DF_STEP_MAX, -DYN_INFINITY)

DFS_SCAN(cummin, DF_STEP_MIN, DYN_INFINITY)

#undef DFS_SCAN

JSValue dyn_df_cumsum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    double* dst;
    uint32_t n, span;
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

    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b, dfs_name[magic])) {
        free(dst);
        return JS_EXCEPTION;
    }
    span = df_map_span(n, b.n, dst);
    dfs_widen(&b, dst, span);
    switch (magic) {
    case DFS_CUMPROD:
        if (mask)
            dfs_cumprod_masked(dst, span, mask);
        else
            dfs_cumprod(dst, span);
        break;
    case DFS_CUMMAX:
        if (mask)
            dfs_cummax_masked(dst, span, mask);
        else
            dfs_cummax(dst, span);
        break;
    case DFS_CUMMIN:
        if (mask)
            dfs_cummin_masked(dst, span, mask);
        else
            dfs_cummin(dst, span);
        break;
    default:
        if (mask)
            dfs_cumsum_masked(dst, span, mask);
        else
            dfs_cumsum(dst, span);
        break;
    }
    return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

JSValue dyn_df_shift(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const double* src;
    double periods, *dst, *tmp = NULL;
    uint32_t n, span, i;
    int idx;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    periods = 1.0;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToFloat64(ctx, &periods, argv[1]))
        return JS_EXCEPTION;
    if (isnan(periods) || periods != floor(periods))
        return JS_ThrowTypeError(ctx, "%s(col, periods): periods must be an "
                                      "integer",
            dfs_name[magic]);

    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b, dfs_name[magic])) {
        free(dst);
        return JS_EXCEPTION;
    }
    span = df_map_span(n, b.n, dst);
    if (!(fabs(periods) < (double)span)) {
        for (i = 0; i < span; i++)
            dst[i] = DYN_NAN;
        return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
            JS_TYPED_ARRAY_FLOAT64);
    }
    if (b.type == DF_F64) {
        src = b.p;
    } else {
        tmp = df_out_alloc(ctx, span, sizeof(double));
        if (!tmp) {
            free(dst);
            return JS_EXCEPTION;
        }
        dfs_widen(&b, tmp, span);
        src = tmp;
    }
    {
        int64_t k = (int64_t)periods, end = (int64_t)span, j;

        int64_t lo = k > 0 ? k : 0;
        int64_t hi = k > 0 ? end : end + k;
        (void)j;
        for (i = 0; i < (uint32_t)lo; i++)
            dst[i] = DYN_NAN;
        for (i = (uint32_t)hi; i < span; i++)
            dst[i] = DYN_NAN;
        if (magic == DFS_DIFF) {
            const double *a = src + lo, *bsrc = src + (lo - k);
            uint32_t m = (uint32_t)(hi - lo);
            double* o = dst + lo;
            for (i = 0; i < m; i++)
                o[i] = a[i] - bsrc[i];
        } else if (hi > lo) {
            memcpy(dst + lo, src + (lo - k),
                (size_t)(hi - lo) * sizeof(double));
        }
    }
    free(tmp);
    return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

JSValue dyn_df_rolling(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    const double* fx;
    double wd, *dst;
    uint32_t i, n, span, w;
    int idx, ok, slot, out_idx = -1;
    JSValueConst maskv = JS_UNDEFINED;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &wd, argc > 1 ? argv[1] : JS_UNDEFINED))
        return JS_EXCEPTION;
    for (slot = 2; slot < argc; slot++) {
        JSValueConst v = argv[slot];
        if (JS_IsUndefined(v) || JS_IsNull(v))
            continue;
        if (out_idx < 0 && df_is_out_bag(ctx, v)) {
            if (df_out_opt(ctx, df, v, dfg_roll_name(magic), &out_idx) < 0)
                return JS_EXCEPTION;
            continue;
        }
        if (JS_IsUndefined(maskv))
            maskv = v;
    }
    mask = df_mask_arg(ctx, maskv, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (!(wd >= 1) || wd != floor(wd) || wd > (double)UINT32_MAX)
        return JS_ThrowRangeError(ctx, "%s: window must be a positive integer, "
                                       "got %g",
            dfg_roll_name(magic), wd);
    w = (uint32_t)wd;

    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b, dfg_roll_name(magic))) {
        free(dst);
        return JS_EXCEPTION;
    }
    span = df_map_span(n, b.n, dst);

    fx = (b.type == DF_F64) ? b.p : NULL;
    switch (magic) {
    case DF_MIN:
        if (dfg_roll_extreme(&b, mask, dst, span, w, 1) == 0)
            break;
        DFG_ROLL(DYN_NAN, if (v == v && (a != a || v < a)) a = v, c ? a : DYN_NAN);
        break;
    case DF_MAX:
        if (dfg_roll_extreme(&b, mask, dst, span, w, 0) == 0)
            break;
        DFG_ROLL(DYN_NAN, if (v == v && (a != a || v > a)) a = v, c ? a : DYN_NAN);
        break;
    case DF_MEAN:
        if (dfg_roll_sum_sel(&b, mask, dst, span, w, 1) == 0)
            break;
        DFG_ROLL(0.0, a += v, c ? a / c : DYN_NAN);
        break;
    default:
        if (dfg_roll_sum_sel(&b, mask, dst, span, w, 0) == 0)
            break;
        DFG_ROLL(0.0, a += v, c ? a : 0.0);
        break;
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

JSValue dyn_df_ema(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    double alpha = 0.0, acc = 0.0, *dst;
    uint32_t i, n, span;
    int seeded = 0, out_idx = -1;

    if (dfo_open(ctx, this_val, argc, argv, 1, "EMA", &df, &b, &alpha, &mask, &out_idx))
        return JS_EXCEPTION;
    if (!(alpha > 0.0) || !(alpha <= 1.0) || isnan(alpha))
        return JS_ThrowRangeError(ctx, "EMA(col, alpha): alpha must be in "
                                       "(0, 1], got %g",
            alpha);
    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    span = df_map_span(n, b.n, dst);
    for (i = 0; i < span; i++) {
        double v;
        if (mask && !mask[i]) {
            dst[i] = seeded ? acc : DYN_NAN;
            continue;
        }
        v = df_get(b.p, b.type, i);
        if (!seeded) {
            acc = v;
            seeded = 1;
        } else
            acc = alpha * v + (1.0 - alpha) * acc;
        dst[i] = acc;
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

JSValue dyn_df_delta_sum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    uint32_t i, span;
    int idx, ok, have = 0;
    double prev = 0.0, s = 0.0;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b, "DELTA_SUM"))
        return JS_EXCEPTION;
    span = b.n < df->nrows ? b.n : df->nrows;
    for (i = 0; i < span; i++) {
        double v;
        if (mask && !mask[i])
            continue;
        v = df_get(b.p, b.type, i);
        if (have && v > prev)
            s += v - prev;
        prev = v;
        have = 1;
    }
    return JS_NewFloat64(ctx, s);
}

JSValue dyn_df_rate(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* op = magic == DFX_IRATE ? "IRATE" : "RATE";
    DataFrame* df;
    DFBound bv, bt;
    const uint8_t* mask;
    uint32_t i, span;
    int iv, it_, ok, have = 0;
    double v0 = 0, t0 = 0, v1 = 0, t1 = 0, pv = 0, pt = 0;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    iv = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (iv < 0)
        return JS_EXCEPTION;
    it_ = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (it_ < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, iv, &bv, op) || df_bind_numeric(ctx, df, it_, &bt, op))
        return JS_EXCEPTION;

    span = bv.n < bt.n ? bv.n : bt.n;
    if (span > df->nrows)
        span = df->nrows;
    for (i = 0; i < span; i++) {
        double v, t;
        if (mask && !mask[i])
            continue;
        v = df_get(bv.p, bv.type, i);
        t = df_get(bt.p, bt.type, i);
        if (!have) {
            v0 = v;
            t0 = t;
            have = 1;
        }
        pv = v1;
        pt = t1;
        v1 = v;
        t1 = t;
        have++;
    }
    if (have < 3)
        return JS_NewFloat64(ctx, DYN_NAN);
    if (magic == DFX_IRATE)
        return JS_NewFloat64(ctx, (v1 - pv) / (t1 - pt));
    return JS_NewFloat64(ctx, (v1 - v0) / (t1 - t0));
}

JSValue dyn_df_rolling_disp(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    const char* op = magic == DFX_ROLL_STD ? "ROLLING_STD" : "ROLLING_VAR";
    double wd = 0.0, *dst;
    uint32_t i, n, span, w;
    uint64_t poll_work = 0;
    int out_idx = -1;

    if (dfo_open(ctx, this_val, argc, argv, 1, op, &df, &b, &wd, &mask, &out_idx))
        return JS_EXCEPTION;
    if (!(wd >= 1) || wd != floor(wd) || wd > (double)UINT32_MAX)
        return JS_ThrowRangeError(ctx, "%s: window must be a positive integer, "
                                       "got %g",
            op, wd);
    w = (uint32_t)wd;
    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    span = df_map_span(n, b.n, dst);

    if (dfg_roll_var_slide(&b, mask, dst, span, w, magic == DFX_ROLL_STD) == 0) {
        if (out_idx < 0)
            return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
                JS_TYPED_ARRAY_FLOAT64);
        if (df_store_out(ctx, df, out_idx, dst, n)) {
            free(dst);
            return JS_EXCEPTION;
        }
        free(dst);
        return JS_DupValue(ctx, this_val);
    }

    i = 0;
    if (!mask && w >= 2 && span >= w + 3) {
        double* own = NULL;
        const double* x = b.type == DF_F64 ? b.p : (own = malloc((size_t)span * sizeof(double) + 1));
        if (x) {
            if (own)
                df_widen(&b, own, span);
            for (; i + 1 < w; i++)
                dst[i] = DYN_NAN;
            for (; i + 3 < span; i += 4) {
                double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
                double q0 = 0.0, q1 = 0.0, q2 = 0.0, q3 = 0.0;
                const double* y = x + (i + 1 - w);
                uint32_t j;
                poll_work += 4 * (uint64_t)w;
                if (poll_work >= DFG_ROLL_POLL_WORK) {
                    poll_work = 0;
                    if (JS_CheckInterrupt(ctx)) {
                        free(own);
                        free(dst);
                        return JS_EXCEPTION;
                    }
                }
                for (j = 0; j < w; j++) {
                    s0 += y[j];
                    s1 += y[j + 1];
                    s2 += y[j + 2];
                    s3 += y[j + 3];
                }
                s0 /= (double)w;
                s1 /= (double)w;
                s2 /= (double)w;
                s3 /= (double)w;
                for (j = 0; j < w; j++) {
                    double d0 = y[j] - s0, d1 = y[j + 1] - s1;
                    double d2 = y[j + 2] - s2, d3 = y[j + 3] - s3;
                    q0 += d0 * d0;
                    q1 += d1 * d1;
                    q2 += d2 * d2;
                    q3 += d3 * d3;
                }
                q0 /= (double)(w - 1);
                q1 /= (double)(w - 1);
                q2 /= (double)(w - 1);
                q3 /= (double)(w - 1);
                if (magic == DFX_ROLL_STD) {
                    q0 = sqrt(q0);
                    q1 = sqrt(q1);
                    q2 = sqrt(q2);
                    q3 = sqrt(q3);
                }
                dst[i] = q0;
                dst[i + 1] = q1;
                dst[i + 2] = q2;
                dst[i + 3] = q3;
            }
            free(own);
        }
    }
    for (; i < span; i++) {
        double mean = 0.0, m2 = 0.0;
        uint32_t c = 0, j, lo;
        if (i + 1 < w) {
            dst[i] = DYN_NAN;
            continue;
        }
        poll_work += w;
        if (poll_work >= DFG_ROLL_POLL_WORK) {
            poll_work = 0;
            if (JS_CheckInterrupt(ctx)) {
                free(dst);
                return JS_EXCEPTION;
            }
        }
        lo = i + 1 - w;
        for (j = lo; j <= i; j++) {
            if (mask && !mask[j])
                continue;
            mean += df_get(b.p, b.type, j);
            c++;
        }
        if (c < 2) {
            dst[i] = DYN_NAN;
            continue;
        }
        mean /= (double)c;
        for (j = lo; j <= i; j++) {
            double d;
            if (mask && !mask[j])
                continue;
            d = df_get(b.p, b.type, j) - mean;
            m2 += d * d;
        }
        m2 /= (double)(c - 1);
        dst[i] = magic == DFX_ROLL_STD ? sqrt(m2) : m2;
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

JSValue dyn_df_pct_change(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    double pd = 1.0, *dst;
    uint32_t i, n, span, p;
    int out_idx = -1;

    if (dfo_open(ctx, this_val, argc, argv, 1, "PCT_CHANGE", &df, &b, &pd, &mask, &out_idx))
        return JS_EXCEPTION;
    if (argc < 2 || JS_IsUndefined(argv[1]) || (argc > 1 && df_is_out_bag(ctx, argv[1])))
        pd = 1.0;
    if (!(pd >= 1) || pd != floor(pd) || pd > (double)UINT32_MAX)
        return JS_ThrowRangeError(ctx, "PCT_CHANGE: periods must be a positive "
                                       "integer, got %g",
            pd);
    p = (uint32_t)pd;
    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    span = df_map_span(n, b.n, dst);

    for (i = 0; i < span; i++) {
        double prev, cur;
        if (i < p || (mask && (!mask[i] || !mask[i - p]))) {
            dst[i] = DYN_NAN;
            continue;
        }
        prev = df_get(b.p, b.type, i - p);
        cur = df_get(b.p, b.type, i);
        dst[i] = (cur - prev) / prev;
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

JSValue dyn_df_delta_sum_ts(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound bv, bt;
    const uint8_t* mask;
    DfwPair* p = NULL;
    uint32_t i, span, n = 0;
    int iv, it_, ok, asc = 1;
    double s = 0.0;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    iv = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (iv < 0)
        return JS_EXCEPTION;
    it_ = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (it_ < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, iv, &bv, "DELTA_SUM_TIMESTAMP") || df_bind_numeric(ctx, df, it_, &bt, "DELTA_SUM_TIMESTAMP"))
        return JS_EXCEPTION;

    span = bv.n < bt.n ? bv.n : bt.n;
    if (span > df->nrows)
        span = df->nrows;
    p = df_out_alloc(ctx, span ? span : 1, sizeof(*p));
    if (!p)
        return JS_EXCEPTION;
    for (i = 0; i < span; i++) {
        double tv;
        if (mask && !mask[i])
            continue;
        tv = df_get(bt.p, bt.type, i);
        if (tv != tv)
            continue;
        if (n && tv < p[n - 1].key)
            asc = 0;
        p[n].key = tv;
        p[n].w = df_get(bv.p, bv.type, i);
        n++;
    }
    if (!asc && dfo_pair_sort(p, n, 512u) && dfw_sort_stable(p, n)) {
        free(p);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 1; i < n; i++)
        if (p[i].w > p[i - 1].w)
            s += p[i].w - p[i - 1].w;
    free(p);
    return JS_NewFloat64(ctx, s);
}

static int dfr_agg(const char* s)
{
    if (!strcmp(s, "sum"))
        return DFR_SUM;
    if (!strcmp(s, "mean"))
        return DFR_MEAN;
    if (!strcmp(s, "min"))
        return DFR_MIN;
    if (!strcmp(s, "max"))
        return DFR_MAX;
    if (!strcmp(s, "count"))
        return DFR_COUNT;
    return -1;
}

JSValue dyn_df_resample(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* op = "RESAMPLE";
    DataFrame* df;
    DFBound b;
    double *times = NULL, *bstart = NULL, *vals = NULL;
    double interval, t0;
    uint32_t* kidx = NULL;
    uint32_t i, n, nb = 0;
    int ti, agg = DFR_SUM;
    const char* aggs = NULL;
    DFBuilder B = { 0 };
    JSValue res;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ti = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ti < 0)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &interval, argc > 1 ? argv[1] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (!(interval > 0.0) || !isfinite(interval))
        return JS_ThrowRangeError(ctx, "%s(time, interval[, agg]): interval "
                                       "must be finite and positive, got %g",
            op, interval);
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        aggs = JS_ToCString(ctx, argv[2]);
        if (!aggs)
            return JS_EXCEPTION;
        agg = dfr_agg(aggs);
        JS_FreeCString(ctx, aggs);
        if (agg < 0)
            return JS_ThrowRangeError(ctx, "%s(..., agg): agg must be one of "
                                           "sum/mean/min/max/count",
                op);
    }
    if (df_bind_numeric(ctx, df, ti, &b, op))
        return JS_EXCEPTION;

    n = b.n < df->nrows ? b.n : df->nrows;
    times = df_out_alloc(ctx, n ? n : 1, sizeof(double));
    if (!times)
        return JS_EXCEPTION;
    for (i = 0; i < n; i++)
        times[i] = df_get(b.p, b.type, i);
    for (i = 1; i < n; i++)
        if (times[i] < times[i - 1]) {
            free(times);
            return JS_ThrowRangeError(ctx, "%s: the time column must be sorted "
                                           "ascending",
                op);
        }
    for (i = 0; i < n; i++)
        if (times[i] != times[i]) {
            free(times);
            return JS_ThrowRangeError(ctx, "%s: the time column must not "
                                           "contain NaN",
                op);
        }
    if (n == 0) {
        double* e1 = df_out_alloc(ctx, 1, sizeof(double));
        double* e2 = df_out_alloc(ctx, 1, sizeof(double));
        if (!e1 || !e2) {
            free(e1);
            free(e2);
            goto fail;
        }
        if (dfb_init(ctx, &B, op, 2, 0)) {
            free(e1);
            free(e2);
            goto fail;
        }
        if (dfb_add(ctx, &B, "bucket", DF_F64, e1, 0)) {
            e1 = NULL;
            goto fail;
        }
        if (dfb_add(ctx, &B, "value", DF_F64, e2, 0)) {
            e2 = NULL;
            goto fail;
        }
        res = dfb_seal(ctx, &B);
        free(times);
        return res;
    }
    t0 = floor(times[0] / interval) * interval;

    kidx = df_out_alloc(ctx, n, sizeof(uint32_t));
    if (!kidx)
        goto fail;
    {
        double* bk = df_out_alloc(ctx, n, sizeof(double));
        if (!bk)
            goto fail;
        nb = 0;
        for (i = 0; i < n; i++) {
            double s = fma(floor((times[i] - t0) / interval), interval, t0);
            if (nb == 0 || s != bk[nb - 1]) {
                bk[nb] = s;
                nb++;
            }
            kidx[i] = nb - 1;
        }
        bstart = malloc((size_t)(nb ? nb : 1) * sizeof(double));
        if (!bstart) {
            free(bk);
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        memcpy(bstart, bk, (size_t)nb * sizeof(double));
        free(bk);
    }

    vals = calloc((size_t)(nb ? nb : 1), sizeof(double));
    if (!vals)
        goto fail;
    if (agg == DFR_MIN)
        for (i = 0; i < nb; i++)
            vals[i] = DYN_INFINITY;
    if (agg == DFR_MAX)
        for (i = 0; i < nb; i++)
            vals[i] = -DYN_INFINITY;
    for (i = 0; i < n; i++) {
        double v = df_get(b.p, b.type, i);
        uint32_t k = kidx[i];
        switch (agg) {
        case DFR_SUM:
            vals[k] += v;
            break;
        case DFR_MEAN:
            vals[k] += v;
            break;
        case DFR_MIN:
            if (v == v && v < vals[k])
                vals[k] = v;
            break;
        case DFR_MAX:
            if (v == v && v > vals[k])
                vals[k] = v;
            break;
        case DFR_COUNT:
            vals[k] += 1.0;
            break;
        }
    }
    if (agg == DFR_MIN || agg == DFR_MAX) {
        double* cnt = calloc((size_t)(nb ? nb : 1), sizeof(double));
        if (!cnt)
            goto fail;
        for (i = 0; i < n; i++)
            if (df_get(b.p, b.type, i) == df_get(b.p, b.type, i))
                cnt[kidx[i]] += 1.0;
        for (i = 0; i < nb; i++)
            if (cnt[i] == 0)
                vals[i] = DYN_NAN;
        free(cnt);
    }
    if (agg == DFR_MEAN) {
        double* cnt = calloc((size_t)(nb ? nb : 1), sizeof(double));
        if (!cnt)
            goto fail;
        for (i = 0; i < n; i++)
            cnt[kidx[i]] += 1.0;
        for (i = 0; i < nb; i++)
            vals[i] = cnt[i] ? vals[i] / cnt[i] : DYN_NAN;
        free(cnt);
    }
    for (i = 0; i < n; i++) {
        double v = df_get(b.p, b.type, i);
        if (v != v && (agg == DFR_SUM || agg == DFR_MEAN))
            vals[kidx[i]] = DYN_NAN;
    }

    if (dfb_init(ctx, &B, op, 2, nb))
        goto fail;
    if (dfb_add(ctx, &B, "bucket", DF_F64, bstart, nb)) {
        bstart = NULL;
        goto fail;
    }
    if (dfb_add(ctx, &B, "value", DF_F64, vals, nb)) {
        vals = NULL;
        goto fail;
    }
    free(times);
    free(kidx);
    res = dfb_seal(ctx, &B);
    return res;
fail:
    free(times);
    free(bstart);
    free(vals);
    free(kidx);
    dfb_free(ctx, &B);
    return JS_EXCEPTION;
}

#endif
