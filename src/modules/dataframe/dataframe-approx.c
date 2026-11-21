#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

#define DFA_HLL_PRECISION 14
#define DFA_SS_COUNTERS_PER_K 8u
#define DFA_SS_MIN_COUNTERS 256u
#define DFA_SS_MAX_COUNTERS 8192u
#define DFA_TOPK_MAX_K 1024

JSValue dyn_df_unique(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DfcSet set;
    const uint8_t* mask;
    int idx, ok;
    uint32_t i;
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
    if (df->cols[idx].type == DF_STR) {
        out = dfc_keys_array(ctx, &df->cols[idx], &set, NULL, set.nent);
    } else {
        double* dst = df_out_alloc(ctx, set.nent, sizeof(double));
        if (!dst) {
            dfc_set_free(&set);
            return JS_EXCEPTION;
        }
        for (i = 0; i < set.nent; i++)
            dst[i] = dfc_key_value(set.keys[i]);
        out = df_to_typed_array(ctx, dst, (size_t)set.nent * sizeof(double),
            JS_TYPED_ARRAY_FLOAT64);
    }
    dfc_set_free(&set);
    return out;
}

JSValue dyn_df_approxCountDistinct(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    dyn_hll_t* hll;
    int idx, ok;
    uint32_t i, any = 0;
    double est;

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
        return dfa_exact_distinct_str(ctx, &df->cols[idx], &b, mask);

    hll = dyn_hll_new(DFA_HLL_PRECISION);
    if (!hll)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < b.n; i++) {
        if (mask && !mask[i])
            continue;
        dyn_hll_add_hash(hll,
            dfa_hash_bits(dfa_canon_bits(df_get(b.p, b.type, i))));
        any = 1;
    }
    est = any ? dyn_hll_count(hll) : 0.0;
    dyn_hll_free(hll);
    return JS_NewFloat64(ctx, est);
}

JSValue dyn_df_approxTopK(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    dfa_ss s;
    const uint8_t* mask;
    double* vals;
    JSValue keys;
    int64_t k64 = 0;
    int idx, ok;
    uint32_t i, k, m, nout;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (JS_ToInt64(ctx, &k64, argc > 1 ? argv[1] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (k64 < 1 || k64 > DFA_TOPK_MAX_K)
        return JS_ThrowRangeError(ctx, "k must be an integer in [1, %d]",
            DFA_TOPK_MAX_K);
    k = (uint32_t)k64;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dyn_df_bind(ctx, df, idx, &b))
        return JS_EXCEPTION;
    if (b.type == DF_STR)
        return dfa_exact_topk_str(ctx, &df->cols[idx], &b, mask, k);

    m = k * DFA_SS_COUNTERS_PER_K;
    if (m < DFA_SS_MIN_COUNTERS)
        m = DFA_SS_MIN_COUNTERS;
    if (m > DFA_SS_MAX_COUNTERS)
        m = DFA_SS_MAX_COUNTERS;
    while (m & (m - 1))
        m += m & (uint32_t)(-(int32_t)m);
    if (dfa_ss_init(&s, m))
        return JS_ThrowOutOfMemory(ctx);

    for (i = 0; i < b.n; i++) {
        if (mask && !mask[i])
            continue;
        dfa_ss_add(&s, dfa_canon_bits(df_get(b.p, b.type, i)));
    }
    qsort(s.c, s.n, sizeof(*s.c), dfa_ss_cmp);

    nout = k < s.n ? k : s.n;
    vals = malloc((size_t)(nout ? nout : 1) * sizeof(double));
    if (!vals) {
        dfa_ss_free(&s);
        return JS_ThrowOutOfMemory(ctx);
    }
    keys = JS_NewArray(ctx);
    for (i = 0; i < nout && !JS_IsException(keys); i++) {
        JSValue key = JS_NewFloat64(ctx, dfa_bits_value(s.c[i].bits));
        vals[i] = (double)s.c[i].count;
        if (JS_IsException(key) || JS_DefinePropertyValueUint32(ctx, keys, i, key, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, keys);
            keys = JS_EXCEPTION;
        }
    }
    dfa_ss_free(&s);
    return dfa_result_pair(ctx, keys, vals, nout);
}

JSValue dyn_df_approxSimilarity(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound ba, bb;
    const uint8_t* mask;
    dfa_kmv* sk;
    int ia, ib, ok;
    uint32_t i;
    double j;

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

    sk = malloc(2 * sizeof(*sk));
    if (!sk)
        return JS_ThrowOutOfMemory(ctx);
    sk[0].n = 0;
    sk[1].n = 0;
    for (i = 0; i < ba.n; i++) {
        if (mask && !mask[i])
            continue;
        dfa_kmv_add(&sk[0], dfa_row_hash(&df->cols[ia], &ba, i));
    }
    for (i = 0; i < bb.n; i++) {
        if (mask && !mask[i])
            continue;
        dfa_kmv_add(&sk[1], dfa_row_hash(&df->cols[ib], &bb, i));
    }
    j = dfa_kmv_jaccard(&sk[0], &sk[1]);
    free(sk);
    return JS_NewFloat64(ctx, j);
}

JSValue dyn_df_uniq_up_to(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    DfoItem* it = NULL;
    double cap = 0.0;
    uint32_t n, m, i, seen = 0, lim;

    if (dfo_open(ctx, this_val, argc, argv, 1, "UNIQ_UP_TO", &df, &b, &cap,
            &mask, NULL))
        return JS_EXCEPTION;
    if (!(cap >= 0.0) || cap != floor(cap) || cap > 65536.0)
        return JS_ThrowRangeError(ctx, "UNIQ_UP_TO(col, n): n must be an "
                                       "integer in [0, 65536], got %g",
            cap);
    lim = (uint32_t)cap;
    {
        uint32_t nslot = 1, probe;
        double* slot;
        uint8_t* used;
        uint32_t span = b.n < df->nrows ? b.n : df->nrows;
        while (nslot < (lim + 1) * 2u)
            nslot <<= 1;
        slot = malloc((size_t)nslot * sizeof(*slot));
        used = calloc(nslot, 1);
        if (!slot || !used) {
            free(slot);
            free(used);
            return JS_ThrowOutOfMemory(ctx);
        }
        int saw_nan = 0;
        for (i = 0; i < span && seen <= lim; i++) {
            double v;
            uint64_t bits;
            if (mask && !mask[i])
                continue;
            v = df_get(b.p, b.type, i);
            if (v != v) {
                if (!saw_nan) {
                    saw_nan = 1;
                    seen++;
                }
                continue;
            }
            if (v == 0.0)
                v = 0.0;
            memcpy(&bits, &v, sizeof(bits));
            bits ^= bits >> 33;
            bits *= 0xff51afd7ed558ccdULL;
            bits ^= bits >> 33;
            probe = (uint32_t)bits & (nslot - 1);
            while (used[probe] && slot[probe] != v)
                probe = (probe + 1) & (nslot - 1);
            if (!used[probe]) {
                used[probe] = 1;
                slot[probe] = v;
                seen++;
            }
        }
        free(slot);
        free(used);
    }
    (void)it;
    (void)n;
    (void)m;
    return JS_NewInt64(ctx, (int64_t)seen);
}

JSValue dyn_df_histogram(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    double nb = 0.0, lo = DYN_INFINITY, hi = -DYN_INFINITY, w;
    double *edges = NULL, *counts = NULL;
    uint32_t nbins, i, span, kept = 0;
    JSValue res, ev, cv;

    if (dfo_open(ctx, this_val, argc, argv, 1,
            magic ? "HISTOGRAM_NORMALIZED" : "HISTOGRAM",
            &df, &b, &nb, &mask, NULL))
        return JS_EXCEPTION;
    if (!(nb >= 1.0) || nb != floor(nb) || nb > 1048576.0)
        return JS_ThrowRangeError(ctx, "%s(col, bins): bins must be a positive "
                                       "integer, got %g",
            magic ? "HISTOGRAM_NORMALIZED" : "HISTOGRAM",
            nb);
    nbins = (uint32_t)nb;
    span = b.n < df->nrows ? b.n : df->nrows;

    for (i = 0; i < span; i++) {
        double v;
        if (mask && !mask[i])
            continue;
        v = df_get(b.p, b.type, i);
        if (v != v || !isfinite(v))
            continue;
        kept++;
        if (v < lo)
            lo = v;
        if (v > hi)
            hi = v;
    }
    edges = df_out_alloc(ctx, nbins + 1, sizeof(double));
    counts = edges ? df_out_alloc(ctx, nbins, sizeof(double)) : NULL;
    if (!counts) {
        free(edges);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nbins; i++)
        counts[i] = 0.0;
    if (kept == 0) {
        lo = 0.0;
        hi = 1.0;
    } else if (hi <= lo) {
        hi = lo + 1.0;
    }
    w = (hi - lo) / (double)nbins;
    if (!isfinite(w) || w <= 0.0) {
        free(edges);
        free(counts);
        return JS_ThrowRangeError(ctx, "HISTOGRAM: the value range is too wide "
                                       "for finite edges");
    }
    for (i = 0; i <= nbins; i++)
        edges[i] = lo + w * (double)i;

    for (i = 0; i < span; i++) {
        double v;
        uint32_t k;
        if (mask && !mask[i])
            continue;
        v = df_get(b.p, b.type, i);
        if (v != v || !isfinite(v))
            continue;
        {
            double r = (v - lo) / w;
            if (!(r >= 0.0))
                k = 0;
            else if (r >= (double)nbins)
                k = nbins - 1;
            else
                k = (uint32_t)r;
        }
        counts[k] += 1.0;
    }
    if (magic && kept)
        for (i = 0; i < nbins; i++)
            counts[i] /= (double)kept;

    res = JS_NewObject(ctx);
    if (JS_IsException(res)) {
        free(edges);
        free(counts);
        return res;
    }
    ev = df_to_typed_array(ctx, edges, (size_t)(nbins + 1) * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
    cv = df_to_typed_array(ctx, counts, (size_t)nbins * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
    if (JS_IsException(ev) || JS_IsException(cv) || JS_DefinePropertyValueStr(ctx, res, "edges", ev, JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueStr(ctx, res, "counts", cv, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

static uint64_t df_splitmix64(uint64_t* st)
{
    uint64_t z = (*st += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

JSValue dyn_df_sample(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBuilder B = { 0 };
    uint32_t *rows = NULL, *shuf = NULL;
    uint32_t i, n, nrows;
    double want = 0;
    uint64_t seed;
    JSValue res;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &want, argc > 0 ? argv[0] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (!(want >= 0) || !isfinite(want) || want != floor(want))
        return JS_ThrowRangeError(ctx, "SAMPLE(n[, seed]): n must be a "
                                       "non-negative integer, got %g",
            want);
    nrows = df->nrows;
    if (want > (double)nrows)
        return JS_ThrowRangeError(ctx, "SAMPLE(n[, seed]): n (%g) exceeds the "
                                       "row count (%u)",
            want, nrows);
    n = (uint32_t)want;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        uint32_t s32;
        if (JS_ToUint32(ctx, &s32, argv[1]))
            return JS_EXCEPTION;
        seed = s32;
    } else {
        seed = 0x9e3779b97f4a7c15ULL ^ (uint64_t)(uintptr_t)&nrows;
        seed ^= (uint64_t)(unsigned)time(NULL) << 17;
        seed ^= (uint64_t)df_getpid();
    }
    rows = df_out_alloc(ctx, n ? n : 1, sizeof(uint32_t));
    shuf = df_out_alloc(ctx, nrows ? nrows : 1, sizeof(uint32_t));
    if (!rows || !shuf) {
        free(rows);
        free(shuf);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nrows; i++)
        shuf[i] = i;
    for (i = 0; i < n; i++) {
        uint32_t j = i + (uint32_t)(df_splitmix64(&seed) % (uint64_t)(nrows - i));
        uint32_t tmp = shuf[i];
        shuf[i] = shuf[j];
        shuf[j] = tmp;
    }
    for (i = 0; i < n; i++)
        rows[i] = shuf[i];
    free(shuf);
    if (dfb_init(ctx, &B, "SAMPLE", df->ncols, n))
        goto fail;
    for (i = 0; i < df->ncols; i++)
        if (dfb_add_gather(ctx, &B, df->cols[i].name, df, (int)i,
                df->cols[i].type, rows, n))
            goto fail;
    free(rows);
    res = dfb_seal(ctx, &B);
    return res;
fail:
    free(rows);
    dfb_free(ctx, &B);
    return JS_EXCEPTION;
}

JSValue dyn_df_bounding_ratio(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound bx, by;
    const uint8_t* mask;
    uint32_t i, span;
    int ix, iy, ok, have = 0;
    double xlo = 0, xhi = 0, ylo = 0, yhi = 0;

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
    if (df_bind_numeric(ctx, df, ix, &bx, "BOUNDING_RATIO") || df_bind_numeric(ctx, df, iy, &by, "BOUNDING_RATIO"))
        return JS_EXCEPTION;

    span = bx.n < by.n ? bx.n : by.n;
    if (span > df->nrows)
        span = df->nrows;
    for (i = 0; i < span; i++) {
        double x, y;
        if (mask && !mask[i])
            continue;
        x = df_get(bx.p, bx.type, i);
        y = df_get(by.p, by.type, i);
        if (x != x || y != y)
            continue;
        if (!have || x < xlo) {
            xlo = x;
            ylo = y;
        }
        if (!have || x > xhi) {
            xhi = x;
            yhi = y;
        }
        have = 1;
    }
    if (!have || xhi == xlo)
        return JS_NewFloat64(ctx, DYN_NAN);
    return JS_NewFloat64(ctx, (yhi - ylo) / (xhi - xlo));
}

#endif
