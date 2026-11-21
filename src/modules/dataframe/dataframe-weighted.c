#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

static int dfw_cmp_w(const void* pa, const void* pb)
{
    const DfwPair *a = pa, *b = pb;

    if (a->w != b->w)
        return a->w > b->w ? -1 : 1;
    return a->key < b->key ? -1 : (a->key > b->key ? 1 : 0);
}

typedef struct {
    double key;
    uint32_t ix;
    uint32_t pad;
} DfwKeyIx;

static inline int dfw_ki_lt(const DfwKeyIx* a, const DfwKeyIx* b)
{
    return a->key < b->key || (a->key == b->key && a->ix < b->ix);
}

static void dfw_ki_sort(DfwKeyIx* p, uint32_t n)
{
    while (n > 24) {
        DfwKeyIx pv;
        uint32_t i = 0, j = n - 1, m = n >> 1;
        if (dfw_ki_lt(&p[m], &p[0])) {
            DfwKeyIx t = p[m];
            p[m] = p[0];
            p[0] = t;
        }
        if (dfw_ki_lt(&p[n - 1], &p[m])) {
            DfwKeyIx t = p[n - 1];
            p[n - 1] = p[m];
            p[m] = t;
        }
        if (dfw_ki_lt(&p[m], &p[0])) {
            DfwKeyIx t = p[m];
            p[m] = p[0];
            p[0] = t;
        }
        pv = p[m];
        for (;;) {
            while (dfw_ki_lt(&p[i], &pv))
                i++;
            while (dfw_ki_lt(&pv, &p[j]))
                j--;
            if (i >= j)
                break;
            {
                DfwKeyIx t = p[i];
                p[i] = p[j];
                p[j] = t;
            }
            i++;
            if (j == 0)
                break;
            j--;
        }
        {
            uint32_t leftn = j + 1, rightn = n - leftn;
            if (leftn < rightn) {
                dfw_ki_sort(p, leftn);
                p += leftn;
                n = rightn;
            } else {
                dfw_ki_sort(p + leftn, rightn);
                n = leftn;
            }
        }
    }
    {
        uint32_t a, b;
        for (a = 1; a < n; a++) {
            DfwKeyIx v = p[a];
            for (b = a; b > 0 && dfw_ki_lt(&v, &p[b - 1]); b--)
                p[b] = p[b - 1];
            p[b] = v;
        }
    }
}

int dfw_sort_stable(DfwPair* p, uint32_t n)
{
    DfwKeyIx* k;
    double* w;
    uint32_t i;

    if (n < 2)
        return 0;
    k = malloc((size_t)n * (sizeof(*k) + sizeof(*w)));
    if (!k)
        return -1;
    w = (double*)(k + n);
    for (i = 0; i < n; i++) {
        k[i].key = p[i].key;
        k[i].ix = i;
        k[i].pad = 0;
        w[i] = p[i].w;
    }
    dfw_ki_sort(k, n);
    for (i = 0; i < n; i++) {
        p[i].key = k[i].key;
        p[i].w = w[k[i].ix];
    }
    free(k);
    return 0;
}

void dfw_sort_k(DfwPair* p, uint32_t n)
{
    while (n > 24) {
        DfwPair pv;
        uint32_t i = 0, j = n - 1, m = n >> 1;
        if (p[m].key < p[0].key) {
            DfwPair t = p[m];
            p[m] = p[0];
            p[0] = t;
        }
        if (p[n - 1].key < p[m].key) {
            DfwPair t = p[n - 1];
            p[n - 1] = p[m];
            p[m] = t;
        }
        if (p[m].key < p[0].key) {
            DfwPair t = p[m];
            p[m] = p[0];
            p[0] = t;
        }
        pv = p[m];
        for (;;) {
            while (p[i].key < pv.key)
                i++;
            while (pv.key < p[j].key)
                j--;
            if (i >= j)
                break;
            {
                DfwPair t = p[i];
                p[i] = p[j];
                p[j] = t;
            }
            i++;
            if (j == 0)
                break;
            j--;
        }
        {
            uint32_t leftn = j + 1, rightn = n - leftn;
            if (leftn < rightn) {
                dfw_sort_k(p, leftn);
                p += leftn;
                n = rightn;
            } else {
                dfw_sort_k(p + leftn, rightn);
                n = leftn;
            }
        }
    }
    {
        uint32_t a, b;
        for (a = 1; a < n; a++) {
            DfwPair v = p[a];
            for (b = a; b > 0 && v.key < p[b - 1].key; b--)
                p[b] = p[b - 1];
            p[b] = v;
        }
    }
}

DfwPair* dfw_collect(JSContext* ctx, const DFBound* b, const DFBound* w,
    const uint8_t* mask, uint32_t nrows, uint32_t* pn, const char* op)
{
    uint32_t span = b->n < nrows ? b->n : nrows, i, n = 0, cap = 16, sh = 60, *tab;
    DfwPair* p;

    if (w && w->n < span)
        span = w->n;
    p = df_out_alloc(ctx, span ? span : 1, sizeof(*p));
    if (!p)
        return NULL;
    while ((uint64_t)cap < (uint64_t)span * 2u && cap < (1u << 31)) {
        cap <<= 1;
        sh--;
    }
    tab = malloc((size_t)cap * sizeof(*tab));
    if (!tab) {
        free(p);
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    memset(tab, 0xff, (size_t)cap * sizeof(*tab));
    for (i = 0; i < span; i++) {
        double v, wt;
        if (mask && !mask[i])
            continue;
        v = df_get(b->p, b->type, i);
        if (v != v)
            continue;
        wt = w ? df_get(w->p, w->type, i) : 1.0;
        if (!isfinite(wt)) {
            JS_ThrowRangeError(ctx, "%s: weight column contains non-finite "
                                    "values",
                op);
            free(p);
            free(tab);
            return NULL;
        }
        {
            uint64_t u;
            uint32_t h;
            if (v == 0.0)
                v = 0.0;
            memcpy(&u, &v, sizeof(u));
            h = (uint32_t)((u * 0x9E3779B97F4A7C15ull) >> sh);
            while (tab[h] != UINT32_MAX && p[tab[h]].key != v)
                h = (h + 1) & (cap - 1);
            if (tab[h] != UINT32_MAX) {
                p[tab[h]].w += wt;
                continue;
            }
            tab[h] = n;
            p[n].key = v;
            p[n].w = wt;
            n++;
        }
    }
    free(tab);
    dfw_sort_k(p, n);
    *pn = n;
    return p;
}

JSValue dyn_df_weighted_top(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const nm[3] = { "TOP_K_WEIGHTED", "APPROX_TOP_SUM",
        "ANY_HEAVY" };
    DataFrame* df;
    DFBound b, wb;
    const uint8_t* mask;
    DfwPair* p = NULL;
    double kd = 1.0, total = 0.0, *out;
    uint32_t n = 0, i, k, nout;
    int idx, widx = -1, wa = 0, ok, wants_k = (magic != DFW_ANY_HEAVY);
    JSValue keys, vals;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNumber(argv[1])) {
        widx = df_col_arg(ctx, df, argv[1]);
        if (widx < 0)
            return JS_EXCEPTION;
        wa = 1;
    } else if (argc > 2 && JS_IsUndefined(argv[1])) {
        wa = 1;
    }
    if (wants_k) {
        int ka = wa ? 2 : 1;
        if (JS_ToFloat64(ctx, &kd, argc > ka ? argv[ka] : JS_UNDEFINED))
            return JS_EXCEPTION;
        if (!(kd >= 1.0) || kd != floor(kd) || kd > 65536.0)
            return JS_ThrowRangeError(ctx, "%s: k must be an integer in "
                                           "[1, 65536], got %g",
                nm[magic], kd);
    }
    {
        int ma = wa + (wants_k ? 1 : 0) + 1;
        mask = df_mask_arg(ctx, argc > ma ? argv[ma] : JS_UNDEFINED,
            df->nrows, &ok);
        if (!ok)
            return JS_EXCEPTION;
    }
    if (df_bind_numeric(ctx, df, idx, &b, nm[magic]))
        return JS_EXCEPTION;
    if (widx >= 0 && df_bind_numeric(ctx, df, widx, &wb, nm[magic]))
        return JS_EXCEPTION;

    p = dfw_collect(ctx, &b, widx >= 0 ? &wb : NULL, mask, df->nrows, &n,
        nm[magic]);
    if (!p)
        return JS_EXCEPTION;
    for (i = 0; i < n; i++)
        total += p[i].w;
    qsort(p, n, sizeof(*p), dfw_cmp_w);

    if (magic == DFW_ANY_HEAVY) {
        JSValue r = (n && p[0].w * 2.0 > total)
            ? JS_NewFloat64(ctx, p[0].key)
            : JS_UNDEFINED;
        free(p);
        return r;
    }
    k = (uint32_t)kd;
    nout = n < k ? n : k;
    out = df_out_alloc(ctx, nout ? nout : 1, sizeof(double));
    if (!out) {
        free(p);
        return JS_EXCEPTION;
    }
    keys = JS_NewArray(ctx);
    if (JS_IsException(keys)) {
        free(p);
        free(out);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nout; i++) {
        JSValue kv = JS_NewFloat64(ctx, p[i].key);
        out[i] = p[i].w;
        if (JS_IsException(kv) || JS_DefinePropertyValueUint32(ctx, keys, i, kv, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, keys);
            free(p);
            free(out);
            return JS_EXCEPTION;
        }
    }
    free(p);
    vals = df_to_typed_array(ctx, out, (size_t)nout * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
    return dfc_pair(ctx, keys, vals);
}

#endif
