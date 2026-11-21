#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

#define DFC_EMPTY 0xFFFFFFFFu
#define DFC_MIN_SLOTS 64u

int dfc_group_count(JSContext* ctx, const DataFrame* df, int ki,
    uint32_t* pnkeys, uint32_t* pngroups)
{
    uint32_t ngroups;

    if (df->cols[ki].type == DF_STR) {
        ngroups = df->cols[ki].dict_len;
    } else {
        DFBound b;
        int rc;

        if (dyn_df_bind(ctx, df, ki, &b))
            return -1;
        rc = dfk_group_range(b.type, b.p, b.n, pnkeys, pngroups);
        if (rc == DFK_E_FLOAT_KEY) {
            JS_ThrowTypeError(ctx, "group key must be an integer or string "
                                   "column");
            return -1;
        }
        if (rc == DFK_E_NEG_KEY) {
            JS_ThrowRangeError(ctx, "negative group key");
            return -1;
        }
        if (rc == DFK_E_TOO_MANY) {
            JS_ThrowRangeError(ctx, "too many groups (max %d)", DF_MAX_GROUPS);
            return -1;
        }
        return 0;
    }
    *pnkeys = ngroups;
    *pngroups = ngroups ? ngroups : 1;
    return 0;
}

uint64_t dfc_key(double v)
{
    uint64_t b;

    if (v != v)
        return 0x7ff8000000000000ULL;
    memcpy(&b, &v, sizeof(b));
    if (b == 0x8000000000000000ULL)
        b = 0;
    return b;
}

double dfc_key_value(uint64_t bits)
{
    double v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

void dfc_hash_seed_once(void)
{
    static atomic_flag seeded = ATOMIC_FLAG_INIT;
    uint64_t s;
    if (atomic_flag_test_and_set_explicit(&seeded, memory_order_relaxed))
        return;
    if (dyn_os_entropy(&s, sizeof s) == 0)
        atomic_store_explicit(&dfc_hash_seed, s, memory_order_relaxed);
}

uint32_t dfc_hash(uint64_t k, uint32_t tag)
{
    uint64_t seed = atomic_load_explicit(&dfc_hash_seed, memory_order_relaxed);
    k ^= seed;
    k ^= (uint64_t)tag * 0x9e3779b97f4a7c15ULL;
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return (uint32_t)k;
}

void dfc_set_free(DfcSet* s)
{
    free(s->slots);
    free(s->keys);
    free(s->tags);
    free(s->counts);
    memset(s, 0, sizeof(*s));
}

static int dfc_set_grow(JSContext* ctx, DfcSet* s)
{
    uint32_t nslots = s->nslots ? s->nslots << 1 : DFC_MIN_SLOTS;
    uint32_t cap = nslots >> 1;
    uint32_t *slots, *tags, *counts;
    uint64_t* keys;
    uint32_t i;

    if (s->nent >= (uint32_t)DF_MAX_GROUPS) {
        JS_ThrowRangeError(ctx, "too many distinct values (max %d)",
            DF_MAX_GROUPS);
        return -1;
    }
    slots = malloc((size_t)nslots * sizeof(*slots));
    keys = realloc(s->keys, (size_t)cap * sizeof(*keys));
    if (keys)
        s->keys = keys;
    tags = realloc(s->tags, (size_t)cap * sizeof(*tags));
    if (tags)
        s->tags = tags;
    counts = realloc(s->counts, (size_t)cap * sizeof(*counts));
    if (counts)
        s->counts = counts;
    if (!slots || !keys || !tags || !counts) {
        free(slots);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    memset(slots, 0xFF, (size_t)nslots * sizeof(*slots));
    free(s->slots);
    s->slots = slots;
    s->nslots = nslots;
    s->mask = nslots - 1;
    s->cap_ent = cap;
    for (i = 0; i < s->nent; i++) {
        uint32_t p = dfc_hash(s->keys[i], s->tags[i]) & s->mask;
        while (s->slots[p] != DFC_EMPTY)
            p = (p + 1) & s->mask;
        s->slots[p] = i;
    }
    return 0;
}

int dfc_set_put(JSContext* ctx, DfcSet* s, uint64_t k, uint32_t tag,
    uint32_t* pent, int* pfresh)
{
    uint32_t p;

    if (s->nent >= s->cap_ent && dfc_set_grow(ctx, s))
        return -1;
    p = dfc_hash(k, tag) & s->mask;
    while (s->slots[p] != DFC_EMPTY) {
        uint32_t e = s->slots[p];
        if (s->keys[e] == k && s->tags[e] == tag) {
            s->counts[e]++;
            if (pent)
                *pent = e;
            if (pfresh)
                *pfresh = 0;
            return 0;
        }
        p = (p + 1) & s->mask;
    }
    s->slots[p] = s->nent;
    s->keys[s->nent] = k;
    s->tags[s->nent] = tag;
    s->counts[s->nent] = 1;
    if (pent)
        *pent = s->nent;
    if (pfresh)
        *pfresh = 1;
    s->nent++;
    return 0;
}

int dfc_scan(JSContext* ctx, const DataFrame* df, int idx,
    const uint8_t* mask, DfcSet* set)
{
    DFBound b;
    uint32_t i, n;

    if (dyn_df_bind(ctx, df, idx, &b))
        return -1;
    n = b.n < df->nrows ? b.n : df->nrows;
    double *wv = NULL, *own = NULL;
    if (n) {
        if (b.type == DF_F64)
            wv = DYN_UNCONST(b.p);
        else {
            wv = own = malloc((size_t)n * sizeof(double));
            if (wv)
                df_widen(&b, wv, n);
        }
    }
    for (i = 0; i < n; i++) {
        if (mask && !mask[i])
            continue;
        if (dfc_set_put(ctx, set, dfc_key(wv ? wv[i] : df_get(b.p, b.type, i)), 0,
                NULL, NULL)) {
            free(own);
            return -1;
        }
    }
    {
        free(own);
        return 0;
    }
}

JSValue dfc_key_js(JSContext* ctx, const DFColumn* c, uint64_t bits)
{
    double v = dfc_key_value(bits);

    if (c->type == DF_STR) {
        uint32_t code = (uint32_t)v;
        if (code >= c->dict_len)
            return JS_ThrowRangeError(ctx, "dictionary code %u out of range",
                code);
        return JS_NewString(ctx, c->dict[code]);
    }
    return JS_NewFloat64(ctx, v);
}

JSValue dfc_keys_array(JSContext* ctx, const DFColumn* c,
    const DfcSet* s, const uint32_t* ord, uint32_t n)
{
    JSValue a = JS_NewArray(ctx);
    uint32_t i;

    if (JS_IsException(a))
        return a;
    for (i = 0; i < n; i++) {
        JSValue k = dfc_key_js(ctx, c, s->keys[ord ? ord[i] : i]);
        if (JS_IsException(k) || JS_DefinePropertyValueUint32(ctx, a, i, k, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, a);
            return JS_EXCEPTION;
        }
    }
    return a;
}

JSValue dfc_pair(JSContext* ctx, JSValue keys, JSValue values)
{
    JSValue res;

    if (JS_IsException(keys) || JS_IsException(values)) {
        JS_FreeValue(ctx, keys);
        JS_FreeValue(ctx, values);
        return JS_EXCEPTION;
    }
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) {
        JS_FreeValue(ctx, keys);
        JS_FreeValue(ctx, values);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "keys", keys, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, values);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "values", values,
            JS_PROP_C_W_E)
        < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

static int dfc_cmp_packed(const void* a, const void* b)
{
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

uint32_t* dfc_rank_order(JSContext* ctx, const DfcSet* s)
{
    uint64_t* packed;
    uint32_t* ord;
    uint32_t i;

    ord = df_out_alloc(ctx, s->nent, sizeof(uint32_t));
    if (!ord)
        return NULL;
    packed = df_out_alloc(ctx, s->nent, sizeof(uint64_t));
    if (!packed) {
        free(ord);
        return NULL;
    }
    for (i = 0; i < s->nent; i++)
        packed[i] = ((uint64_t)(0xFFFFFFFFu - s->counts[i]) << 32) | i;
    qsort(packed, s->nent, sizeof(*packed), dfc_cmp_packed);
    for (i = 0; i < s->nent; i++)
        ord[i] = (uint32_t)packed[i];
    free(packed);
    return ord;
}

void dfc_grouped_free(DfcGrouped* G)
{
    free(G->flat);
    free(G->cnt);
    free(G->off);
    memset(G, 0, sizeof(*G));
}

int dfc_group_gather(JSContext* ctx, DataFrame* df, int ki, int vi,
    const uint8_t* mask, int uniq, DfcGrouped* G)
{
    DFBound kb, vb;
    DfcSet set;
    uint8_t* keep = NULL;
    uint32_t i, g, n, total;

    memset(G, 0, sizeof(*G));
    memset(&set, 0, sizeof(set));
    if (dfc_group_count(ctx, df, ki, &G->nkeys, &G->ngroups))
        return -1;
    if (dyn_df_bind(ctx, df, ki, &kb) || dyn_df_bind(ctx, df, vi, &vb))
        return -1;
    if (vb.type == DF_STR) {
        JS_ThrowTypeError(ctx, "cannot collect %s into a group",
            df_type_name(vb.type));
        return -1;
    }
    n = kb.n < vb.n ? kb.n : vb.n;
    if (n > df->nrows)
        n = df->nrows;
    G->n = n;

    G->cnt = calloc(G->ngroups, sizeof(*G->cnt));
    G->off = malloc((size_t)G->ngroups * sizeof(*G->off));
    if (!G->cnt || !G->off) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    if (uniq) {
        keep = calloc(n ? n : 1, 1);
        if (!keep) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
    }

    for (i = 0; i < n; i++) {
        if (mask && !mask[i])
            continue;
        g = (uint32_t)df_get(kb.p, kb.type, i);
        if (g >= G->ngroups)
            continue;
        if (uniq) {
            int fresh;
            if (dfc_set_put(ctx, &set, dfc_key(df_get(vb.p, vb.type, i)), g,
                    NULL, &fresh))
                goto fail;
            if (!fresh)
                continue;
            keep[i] = 1;
        }
        G->cnt[g]++;
    }

    total = 0;
    for (g = 0; g < G->ngroups; g++) {
        G->off[g] = total;
        total += G->cnt[g];
    }
    G->total = total;
    G->flat = df_out_alloc(ctx, total, sizeof(double));
    if (!G->flat)
        goto fail;

    for (i = 0; i < n; i++) {
        if (mask && !mask[i])
            continue;
        g = (uint32_t)df_get(kb.p, kb.type, i);
        if (g >= G->ngroups)
            continue;
        if (uniq && !keep[i])
            continue;
        G->flat[G->off[g]++] = df_get(vb.p, vb.type, i);
    }
    free(keep);
    dfc_set_free(&set);
    return 0;

fail:
    free(keep);
    dfc_set_free(&set);
    dfc_grouped_free(G);
    return -1;
}

JSValue dfc_values_array(JSContext* ctx, const DfcGrouped* G)
{
    JSValue values = JS_NewArray(ctx);
    uint32_t g;

    if (JS_IsException(values))
        return values;
    for (g = 0; g < G->nkeys; g++) {
        double* slice = df_out_alloc(ctx, G->cnt[g], sizeof(double));
        JSValue ta;
        if (!slice) {
            JS_FreeValue(ctx, values);
            return JS_EXCEPTION;
        }
        memcpy(slice, G->flat + (G->off[g] - G->cnt[g]),
            (size_t)G->cnt[g] * sizeof(double));
        ta = df_to_typed_array(ctx, slice, (size_t)G->cnt[g] * sizeof(double),
            JS_TYPED_ARRAY_FLOAT64);
        if (JS_IsException(ta) || JS_DefinePropertyValueUint32(ctx, values, g, ta, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, values);
            return JS_EXCEPTION;
        }
    }
    return values;
}

JSValue dfc_dense_keys(JSContext* ctx, const DataFrame* df, int ki,
    uint32_t nkeys)
{
    JSValue keys = JS_NewArray(ctx);
    uint32_t g;

    if (JS_IsException(keys))
        return keys;
    for (g = 0; g < nkeys; g++) {
        JSValue k = (df->cols[ki].type == DF_STR)
            ? JS_NewString(ctx, df->cols[ki].dict[g])
            : JS_NewInt64(ctx, g);
        if (JS_IsException(k) || JS_DefinePropertyValueUint32(ctx, keys, g, k, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, keys);
            return JS_EXCEPTION;
        }
    }
    return keys;
}

static int dfc_moving_blocks(double* v, uint32_t m, uint32_t w, int want_avg)
{
    double* suf = malloc((size_t)w * sizeof(double));
    double* out = malloc((size_t)m * sizeof(double));
    double run = 0.0;
    uint32_t s, i;

    if (!suf || !out) {
        free(suf);
        free(out);
        return -1;
    }
    for (i = 0; i + 1 < w; i++) {
        run += v[i];
        out[i] = want_avg ? run / (double)(i + 1) : run;
    }
    for (s = 0; s < m; s += w) {
        uint32_t end = (s + w < m) ? s + w : m;
        uint32_t k, lo, hi;
        double r = 0.0, pre = 0.0;
        for (k = end; k-- > s;) {
            r += v[k];
            suf[k - s] = r;
        }
        lo = s + w - 1;
        hi = s + 2 * w - 2;
        if (hi >= m)
            hi = m - 1;
        for (k = lo; k <= hi; k++) {
            uint32_t st = k + 1 - w;
            double a;
            if (k >= end)
                pre += v[k];
            a = (st < end ? suf[st - s] : 0.0) + pre;
            out[k] = want_avg ? a / (double)w : a;
        }
    }
    memcpy(v, out, (size_t)m * sizeof(double));
    free(out);
    free(suf);
    return 0;
}

void dfc_moving_slice(double* v, uint32_t m, uint32_t w, int want_avg)
{
    uint32_t i;

    if (w == 0 || w >= m) {
        double a = 0.0;
        for (i = 0; i < m; i++) {
            a += v[i];
            v[i] = want_avg ? a / (double)(i + 1) : a;
        }
        return;
    }
    if (w == 1)
        return;
    if (w >= DFG_ROLL_SLIDE_MIN && dfc_moving_blocks(v, m, w, want_avg) == 0)
        return;
    for (i = m; i-- > 0;) {
        uint32_t lo = (i + 1 >= w) ? i + 1 - w : 0, j, c = i - lo + 1;
        double a = 0.0;
        for (j = lo; j <= i; j++)
            a += v[j];
        v[i] = want_avg ? a / (double)c : a;
    }
}

int dfc_set_find(const DfcSet* s, uint64_t k, uint32_t tag, uint32_t* pent)
{
    uint32_t p;

    if (!s->slots || !s->nent)
        return -1;
    p = dfc_hash(k, tag) & s->mask;
    while (s->slots[p] != DFC_EMPTY) {
        uint32_t e = s->slots[p];
        if (s->keys[e] == k && s->tags[e] == tag) {
            if (pent)
                *pent = e;
            return 0;
        }
        p = (p + 1) & s->mask;
    }
    return -1;
}

static JSValue dfc_group_prune(JSContext* ctx, JSValueConst this_val,
    JSValueConst keyname, JSValue res)
{
    DataFrame* df;
    DFBound kb;
    JSValue keys = JS_UNDEFINED, vals = JS_UNDEFINED, nk = JS_UNDEFINED, nv = JS_UNDEFINED;
    uint8_t* seen = NULL;
    uint32_t i, j, g, nkeys, have = 0;
    int64_t len;
    int ki;

    if (JS_IsException(res))
        return res;
    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        goto fail;
    ki = df_col_arg(ctx, df, keyname);
    if (ki < 0)
        goto fail;
    if (df->cols[ki].type != DF_STR)
        return res;
    if (dyn_df_bind(ctx, df, ki, &kb))
        goto fail;
    keys = JS_GetPropertyStr(ctx, res, "keys");
    if (JS_IsException(keys))
        goto fail;
    {
        JSValue lv = JS_GetPropertyStr(ctx, keys, "length");
        if (JS_IsException(lv) || JS_ToInt64(ctx, &len, lv)) {
            JS_FreeValue(ctx, lv);
            goto fail;
        }
        JS_FreeValue(ctx, lv);
    }
    nkeys = (uint32_t)len;
    seen = calloc((size_t)nkeys + 1, 1);
    if (!seen) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    {
        const int32_t* kp = kb.p;
        uint32_t head = kb.n < 4096u ? kb.n : 4096u;
        for (i = 0; i < head && have < nkeys; i++) {
            g = (uint32_t)kp[i];
            if (g < nkeys && !seen[g]) {
                seen[g] = 1;
                have++;
            }
        }
        if (have < nkeys && i < kb.n) {
            for (; i < kb.n; i++) {
                g = (uint32_t)kp[i];
                seen[g < nkeys ? g : nkeys] = 1;
            }
            for (have = 0, g = 0; g < nkeys; g++)
                have += seen[g];
        }
    }
    if (have == nkeys) {
        free(seen);
        JS_FreeValue(ctx, keys);
        return res;
    }
    vals = JS_GetPropertyStr(ctx, res, "values");
    if (JS_IsException(vals))
        goto fail;
    nk = JS_NewArray(ctx);
    if (JS_IsException(nk))
        goto fail;
    if (JS_IsArray(ctx, vals) == 1) {
        nv = JS_NewArray(ctx);
        if (JS_IsException(nv))
            goto fail;
    } else {
        double* buf = malloc((have ? have : 1) * sizeof(double));
        if (!buf) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        for (i = 0, j = 0; i < nkeys; i++) {
            JSValue e;
            if (!seen[i])
                continue;
            e = JS_GetPropertyUint32(ctx, vals, i);
            if (JS_IsException(e) || JS_ToFloat64(ctx, &buf[j], e)) {
                JS_FreeValue(ctx, e);
                free(buf);
                goto fail;
            }
            JS_FreeValue(ctx, e);
            j++;
        }
        nv = df_to_typed_array(ctx, buf, (size_t)have * sizeof(double),
            JS_TYPED_ARRAY_FLOAT64);
        if (JS_IsException(nv))
            goto fail;
    }
    for (i = 0, j = 0; i < nkeys; i++) {
        JSValue e;
        if (!seen[i])
            continue;
        e = JS_GetPropertyUint32(ctx, keys, i);
        if (JS_IsException(e) || JS_DefinePropertyValueUint32(ctx, nk, j, e, JS_PROP_C_W_E) < 0)
            goto fail;
        if (JS_IsArray(ctx, nv) == 1) {
            e = JS_GetPropertyUint32(ctx, vals, i);
            if (JS_IsException(e) || JS_DefinePropertyValueUint32(ctx, nv, j, e, JS_PROP_C_W_E) < 0)
                goto fail;
        }
        j++;
    }
    free(seen);
    JS_FreeValue(ctx, keys);
    JS_FreeValue(ctx, vals);
    if (JS_DefinePropertyValueStr(ctx, res, "keys", nk, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, nv);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "values", nv, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
fail:
    free(seen);
    JS_FreeValue(ctx, keys);
    JS_FreeValue(ctx, vals);
    JS_FreeValue(ctx, nk);
    JS_FreeValue(ctx, nv);
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}

JSValue dfc_grouped(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic, dfc_group_fn* fn)
{
    JSValueConst av[8];
    JSValue key, res;
    int i, n = argc < 8 ? argc : 8;

    if (argc < 1)
        return fn(ctx, this_val, argc, argv, magic);
    key = JS_ToString(ctx, argv[0]);
    if (JS_IsException(key))
        return key;
    for (i = 0; i < n; i++)
        av[i] = argv[i];
    av[0] = key;
    res = dfc_group_prune(ctx, this_val, key, fn(ctx, this_val, n, av, magic));
    JS_FreeValue(ctx, key);
    return res;
}

#endif
