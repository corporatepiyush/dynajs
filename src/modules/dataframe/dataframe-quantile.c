#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

static int df_clz64(uint64_t v)
{
#if defined(_MSC_VER)
    unsigned long i;
    _BitScanReverse64(&i, v);
    return 63 - (int)i;
#else
    return __builtin_clzll(v);
#endif
}

static uint32_t dfv_gather(const DFBound* b, const uint8_t* mask,
    uint32_t span, double* a)
{
    uint32_t i, n = 0;

    if (b->type == DF_F64 && !mask) {
        const double* x = b->p;
        for (i = 0; i < span; i++) {
            a[n] = x[i];
            n += (x[i] == x[i]);
        }
        return n;
    }
    for (i = 0; i < span; i++) {
        double v;
        if (mask && !mask[i])
            continue;
        v = df_get(b->p, b->type, i);
        a[n] = v;
        n += (v == v);
    }
    return n;
}

static uint32_t dfv_gather_keys(const DFBound* b, const uint8_t* mask,
    uint32_t span, uint64_t* k,
    uint64_t* pmin, uint64_t* pmax)
{
    uint64_t lo = ~(uint64_t)0, hi = 0;
    uint32_t i, n = 0;

    if (b->type == DF_F64 && !mask) {
        const double* x = b->p;
        for (i = 0; i < span; i++) {
            uint64_t u = dfo_skey(x[i]);
            k[n] = u;
            if (x[i] == x[i]) {
                n++;
                if (u < lo)
                    lo = u;
                if (u > hi)
                    hi = u;
            }
        }
    } else {
        for (i = 0; i < span; i++) {
            double v;
            uint64_t u;
            if (mask && !mask[i])
                continue;
            v = df_get(b->p, b->type, i);
            u = dfo_skey(v);
            k[n] = u;
            if (v == v) {
                n++;
                if (u < lo)
                    lo = u;
                if (u > hi)
                    hi = u;
            }
        }
    }
    *pmin = lo;
    *pmax = hi;
    return n;
}

static void dfv_insertion(double* a, uint32_t n)
{
    uint32_t i, j;

    for (i = 1; i < n; i++) {
        double v = a[i];
        for (j = i; j > 0 && v < a[j - 1]; j--)
            a[j] = a[j - 1];
        a[j] = v;
    }
}

static double dfv_radix_select(uint64_t* k, uint32_t n, uint32_t rank,
    uint64_t kmin, uint64_t kmax)
{
    uint64_t diff = kmin ^ kmax;
    int level;

    if (!diff || n <= 1)
        return dfo_unskey(k[0]);
    level = (63 - df_clz64(diff)) / 8;
    for (; level >= 0 && n > 1; level--) {
        uint32_t hist[256], i, acc = 0, d, o = 0, sh = (uint32_t)level * 8;
        memset(hist, 0, sizeof(hist));
        for (i = 0; i < n; i++)
            hist[(k[i] >> sh) & 0xff]++;
        for (d = 0; d < 255; d++) {
            if (acc + hist[d] > rank)
                break;
            acc += hist[d];
        }
        if (hist[d] == n)
            continue;
        rank -= acc;
        for (i = 0; i < n; i++) {
            k[o] = k[i];
            o += ((k[i] >> sh) & 0xff) == d;
        }
        n = o;
    }
    return dfo_unskey(k[0]);
}

static double dfv_next_above(const DFBound* b, const uint8_t* mask,
    uint32_t span, double v, uint32_t rank)
{
    uint32_t i, le = 0;
    double nx = DYN_INFINITY;

    if (b->type == DF_F64 && !mask) {
        const double* x = b->p;
        for (i = 0; i < span; i++)
            le += (x[i] <= v);
        if (le >= rank + 2)
            return v;
        for (i = 0; i < span; i++) {
            double c = x[i] > v ? x[i] : DYN_INFINITY;
            nx = fmin(nx, c);
        }
    } else {
        for (i = 0; i < span; i++) {
            double x;
            if (mask && !mask[i])
                continue;
            x = df_get(b->p, b->type, i);
            le += (x <= v);
            if (x > v && x < nx)
                nx = x;
        }
    }
    return le >= rank + 2 ? v : nx;
}

static void dfv_select(double* a, uint32_t n, uint32_t k)
{
    while (n > DFO_SMALL) {
        double p, t;
        int64_t i = -1, j = (int64_t)n;
        uint32_t m = n >> 1, last = n - 1;

        if (a[m] < a[0]) {
            t = a[m];
            a[m] = a[0];
            a[0] = t;
        }
        if (a[last] < a[m]) {
            t = a[last];
            a[last] = a[m];
            a[m] = t;
        }
        if (a[m] < a[0]) {
            t = a[m];
            a[m] = a[0];
            a[0] = t;
        }
        p = a[m];
        for (;;) {
            do {
                i++;
            } while (a[i] < p);
            do {
                j--;
            } while (a[j] > p);
            if (i >= j)
                break;
            t = a[i];
            a[i] = a[j];
            a[j] = t;
        }
        if (k <= (uint32_t)j) {
            n = (uint32_t)j + 1;
        } else {
            a += (uint32_t)j + 1;
            n -= (uint32_t)j + 1;
            k -= (uint32_t)j + 1;
        }
    }
    dfv_insertion(a, n);
}

static double dfq_from_keys(uint64_t* k, uint32_t m, double q, int magic,
    uint64_t kmin, uint64_t kmax,
    const DFBound* b, const uint8_t* mask, uint32_t span)
{
    double pos, frac, lo_v, hi_v;
    uint32_t lo;

    if (m == 0)
        return DYN_NAN;
    if (magic == DFO_Q_DISC) {
        double t = ceil(q * (double)m);
        uint32_t i = t <= 1.0 ? 0 : (uint32_t)t - 1;
        if (i >= m)
            i = m - 1;
        return dfv_radix_select(k, m, i, kmin, kmax);
    }
    pos = q * (double)(m - 1);
    lo = (uint32_t)pos;
    frac = pos - (double)lo;
    lo_v = dfv_radix_select(k, m, lo, kmin, kmax);
    if (frac == 0.0 || lo + 1 >= m)
        return lo_v;
    hi_v = dfv_next_above(b, mask, span, lo_v, lo);
    return lo_v + frac * (hi_v - lo_v);
}

JSValue dyn_df_quantile(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const dfo_q_name[4] = {
        "QUANTILE", "PERCENTILE_CONT", "PERCENTILE_DISC", "MEDIAN"
    };
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    double *a, q = 0.5, res;
    uint32_t span, m;

    if (dfo_open(ctx, this_val, argc, argv, magic != DFO_Q_MEDIAN,
            dfo_q_name[magic], &df, &b, &q, &mask, NULL))
        return JS_EXCEPTION;
    if (magic != DFO_Q_MEDIAN && (isnan(q) || q < 0.0 || q > 1.0))
        return JS_ThrowRangeError(ctx, "%s(col, q): q must be in [0, 1], got %g",
            dfo_q_name[magic], q);
    span = b.n < df->nrows ? b.n : df->nrows;
    if (span >= DFO_RADIX_MIN && (magic == DFO_Q_DISC || (q >= 0.25 && q <= 0.75))) {
        uint64_t *k = df_out_alloc(ctx, span, sizeof(uint64_t)), kmin, kmax;
        if (!k)
            return JS_EXCEPTION;
        m = dfv_gather_keys(&b, mask, span, k, &kmin, &kmax);
        res = dfq_from_keys(k, m, q, magic, kmin, kmax, &b, mask, span);
        free(k);
        return m ? JS_NewFloat64(ctx, res) : JS_UNDEFINED;
    }
    a = df_out_alloc(ctx, span ? span : 1, sizeof(double));
    if (!a)
        return JS_EXCEPTION;
    m = dfv_gather(&b, mask, span, a);

    if (m == 0) {
        free(a);
        return JS_UNDEFINED;
    }
    if (magic == DFO_Q_DISC) {
        double t = ceil(q * (double)m);
        uint32_t i = t <= 1.0 ? 0 : (uint32_t)t - 1;
        if (i >= m)
            i = m - 1;
        dfv_select(a, m, i);
        res = a[i];
    } else {
        double pos = q * (double)(m - 1);
        uint32_t lo = (uint32_t)pos;
        double frac = pos - (double)lo;
        dfv_select(a, m, lo);
        if (frac == 0.0 || lo + 1 >= m) {
            res = a[lo];
        } else {
            dfv_select(a + lo + 1, m - lo - 1, 0);
            res = a[lo] + frac * (a[lo + 1] - a[lo]);
        }
    }
    free(a);
    return JS_NewFloat64(ctx, res);
}

JSValue dyn_df_approxPercentile(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    dfa_tdigest* t;
    double q = 0, v;
    int idx, ok;
    uint32_t i;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &q, argc > 1 ? argv[1] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (!(q >= 0.0 && q <= 1.0))
        return JS_ThrowRangeError(ctx, "q must be a number in [0, 1]");
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dyn_df_bind(ctx, df, idx, &b))
        return JS_EXCEPTION;
    if (b.type == DF_STR)
        return JS_ThrowTypeError(ctx, "cannot reduce a string column");

    t = malloc(sizeof(*t));
    if (!t)
        return JS_ThrowOutOfMemory(ctx);
    dfa_td_init(t);
    for (i = 0; i < b.n; i++) {
        if (mask && !mask[i])
            continue;
        dfa_td_add(t, df_get(b.p, b.type, i));
    }
    if (t->total <= 0.0) {
        free(t);
        return JS_UNDEFINED;
    }
    v = dfa_td_quantile(t, q);
    free(t);
    return JS_NewFloat64(ctx, v);
}

JSValue dyn_df_quantile_lh(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const nm[2] = { "QUANTILE_EXACT_LOW",
        "QUANTILE_EXACT_HIGH" };
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    DfoItem* it = NULL;
    double q = 0.5, pos;
    uint32_t n, m, k;

    if (dfo_open(ctx, this_val, argc, argv, 1, nm[magic], &df, &b, &q, &mask, NULL))
        return JS_EXCEPTION;
    if (isnan(q) || q < 0.0 || q > 1.0)
        return JS_ThrowRangeError(ctx, "%s(col, q): q must be in [0, 1], got %g",
            nm[magic], q);
    if (dfo_gathered(ctx, &b, mask, df->nrows, &it, &n, &m))
        return JS_EXCEPTION;
    if (m == 0) {
        free(it);
        return JS_UNDEFINED;
    }
    pos = q * (double)(m - 1);
    k = magic == DFX_Q_LOW ? (uint32_t)floor(pos) : (uint32_t)ceil(pos);
    if (k >= m)
        k = m - 1;
    dfo_select(it, n, k, 0);
    q = it[k].key;
    free(it);
    return JS_NewFloat64(ctx, q);
}

JSValue dyn_df_quantiles(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    DfoItem* it = NULL;
    JSValue arr, lenv;
    double *qs = NULL, *out = NULL;
    uint32_t n, m, i, nq = 0;
    int idx, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;

    arr = argc > 1 ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED;
    if (!JS_IsObject(arr)) {
        JS_FreeValue(ctx, arr);
        return JS_ThrowTypeError(ctx, "QUANTILES(col, qs): qs must be an array");
    }
    lenv = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_IsException(lenv) || JS_ToUint32(ctx, &nq, lenv) < 0) {
        JS_FreeValue(ctx, lenv);
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lenv);
    if (nq > (1u << 20)) {
        JS_FreeValue(ctx, arr);
        return JS_ThrowRangeError(ctx, "QUANTILES: too many quantiles");
    }
    qs = df_out_alloc(ctx, nq, sizeof(double));
    if (!qs) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nq; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsException(e) || JS_ToFloat64(ctx, &qs[i], e) < 0) {
            JS_FreeValue(ctx, e);
            JS_FreeValue(ctx, arr);
            free(qs);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, e);
        if (isnan(qs[i]) || qs[i] < 0.0 || qs[i] > 1.0) {
            double bad = qs[i];
            JS_FreeValue(ctx, arr);
            free(qs);
            return JS_ThrowRangeError(ctx, "QUANTILES: q must be in [0, 1], "
                                           "got %g",
                bad);
        }
    }
    JS_FreeValue(ctx, arr);

    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok || df_bind_numeric(ctx, df, idx, &b, "QUANTILES")) {
        free(qs);
        return JS_EXCEPTION;
    }
    if (dfo_sorted(ctx, &b, mask, df->nrows, 0, &it, &n)) {
        free(qs);
        return JS_EXCEPTION;
    }
    m = dfo_valued(it, n);
    out = df_out_alloc(ctx, nq, sizeof(double));
    if (!out) {
        free(qs);
        free(it);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nq; i++) {
        if (m == 0) {
            out[i] = DYN_NAN;
        } else {
            double pos = qs[i] * (double)(m - 1), frac;
            uint32_t lo = (uint32_t)pos;
            frac = pos - (double)lo;
            out[i] = (frac == 0.0 || lo + 1 >= m)
                ? it[lo].key
                : it[lo].key + frac * (it[lo + 1].key - it[lo].key);
        }
    }
    free(qs);
    free(it);
    return df_to_typed_array(ctx, out, (size_t)nq * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

JSValue dyn_df_quantiles_td(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    dfa_tdigest* t = NULL;
    JSValue arr, lenv;
    double *qs = NULL, *out = NULL;
    uint32_t i, span, nq = 0;
    int idx, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;

    arr = argc > 1 ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED;
    if (!JS_IsObject(arr)) {
        JS_FreeValue(ctx, arr);
        return JS_ThrowTypeError(ctx,
            "QUANTILES_TDIGEST(col, qs): qs must be an array");
    }
    lenv = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_IsException(lenv) || JS_ToUint32(ctx, &nq, lenv) < 0) {
        JS_FreeValue(ctx, lenv);
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lenv);
    if (nq > (1u << 20)) {
        JS_FreeValue(ctx, arr);
        return JS_ThrowRangeError(ctx, "QUANTILES_TDIGEST: too many quantiles");
    }
    qs = df_out_alloc(ctx, nq, sizeof(double));
    if (!qs) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nq; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsException(e) || JS_ToFloat64(ctx, &qs[i], e) < 0) {
            JS_FreeValue(ctx, e);
            JS_FreeValue(ctx, arr);
            free(qs);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, e);
        if (isnan(qs[i]) || qs[i] < 0.0 || qs[i] > 1.0) {
            double bad = qs[i];
            JS_FreeValue(ctx, arr);
            free(qs);
            return JS_ThrowRangeError(ctx,
                "QUANTILES_TDIGEST: q must be in [0, 1], got %g", bad);
        }
    }
    JS_FreeValue(ctx, arr);

    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok || df_bind_numeric(ctx, df, idx, &b, "QUANTILES_TDIGEST")) {
        free(qs);
        return JS_EXCEPTION;
    }
    t = malloc(sizeof(*t));
    if (!t) {
        free(qs);
        return JS_ThrowOutOfMemory(ctx);
    }
    dfa_td_init(t);
    span = b.n < df->nrows ? b.n : df->nrows;
    for (i = 0; i < span; i++) {
        if (mask && !mask[i])
            continue;
        dfa_td_add(t, df_get(b.p, b.type, i));
    }
    out = df_out_alloc(ctx, nq, sizeof(double));
    if (!out) {
        free(qs);
        free(t);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nq; i++)
        out[i] = t->total > 0.0 ? dfa_td_quantile(t, qs[i]) : DYN_NAN;
    free(qs);
    free(t);
    return df_to_typed_array(ctx, out, (size_t)nq * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

JSValue dyn_df_quantile_weighted(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b, wb;
    const uint8_t* mask;
    DfwPair* p = NULL;
    double q = 0.5, total = 0.0, run = 0.0, target, res;
    uint32_t n = 0, i;
    int idx, widx, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    widx = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (widx < 0)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &q, argc > 2 ? argv[2] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (isnan(q) || q < 0.0 || q > 1.0)
        return JS_ThrowRangeError(ctx, "QUANTILE_EXACT_WEIGHTED(col, w, q): q "
                                       "must be in [0, 1], got %g",
            q);
    mask = df_mask_arg(ctx, argc > 3 ? argv[3] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b, "QUANTILE_EXACT_WEIGHTED") || df_bind_numeric(ctx, df, widx, &wb, "QUANTILE_EXACT_WEIGHTED"))
        return JS_EXCEPTION;

    p = dfw_collect(ctx, &b, &wb, mask, df->nrows, &n,
        "QUANTILE_EXACT_WEIGHTED");
    if (!p)
        return JS_EXCEPTION;
    if (n == 0) {
        free(p);
        return JS_UNDEFINED;
    }
    for (i = 0; i < n; i++)
        total += p[i].w;
    if (!(total > 0.0)) {
        free(p);
        return JS_UNDEFINED;
    }
    target = q * total;
    res = p[n - 1].key;
    for (i = 0; i < n; i++) {
        run += p[i].w;
        if (run >= target) {
            res = p[i].key;
            break;
        }
    }
    free(p);
    return JS_NewFloat64(ctx, res);
}

JSValue dyn_df_quantile_td_weighted(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b, wb;
    const uint8_t* mask;
    dfa_tdigest* t;
    double q = 0, v;
    uint32_t i, span;
    int idx, widx, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    widx = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (widx < 0)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &q, argc > 2 ? argv[2] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (!(q >= 0.0 && q <= 1.0))
        return JS_ThrowRangeError(ctx, "QUANTILE_TDIGEST_WEIGHTED(col, w, q): "
                                       "q must be in [0, 1], got %g",
            q);
    mask = df_mask_arg(ctx, argc > 3 ? argv[3] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b, "QUANTILE_TDIGEST_WEIGHTED") || df_bind_numeric(ctx, df, widx, &wb, "QUANTILE_TDIGEST_WEIGHTED"))
        return JS_EXCEPTION;

    t = malloc(sizeof(*t));
    if (!t)
        return JS_ThrowOutOfMemory(ctx);
    dfa_td_init(t);
    span = b.n < wb.n ? b.n : wb.n;
    if (span > df->nrows)
        span = df->nrows;
    for (i = 0; i < span; i++) {
        if (mask && !mask[i])
            continue;
        dfa_td_add_w(t, df_get(b.p, b.type, i), df_get(wb.p, wb.type, i));
    }
    if (t->total <= 0.0) {
        free(t);
        return JS_UNDEFINED;
    }
    v = dfa_td_quantile(t, q);
    free(t);
    return JS_NewFloat64(ctx, v);
}

#endif
