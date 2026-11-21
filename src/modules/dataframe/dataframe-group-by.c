#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

#define DF_BITMAP_MAX_BITS (1u << 26)

static uint32_t* df_keys_u32(const DFBound* b, uint32_t n)
{
    return dfk_keys_u32(b->type, b->p, n);
}

static JSValue dyn_df_group_by_sum_dense(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound kb, vb;
    const uint8_t* mask;
    int ki, vi, ok;
    uint32_t i, ngroups, nkeys;
    double* acc = NULL;
    JSValue keys = JS_UNDEFINED, vals = JS_UNDEFINED, res = JS_UNDEFINED;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ki = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ki < 0)
        return JS_EXCEPTION;
    vi = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (vi < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dfc_group_count(ctx, df, ki, &nkeys, &ngroups))
        return JS_EXCEPTION;

    acc = calloc(ngroups, sizeof(double));
    if (!acc)
        return JS_ThrowOutOfMemory(ctx);

    if (dyn_df_bind(ctx, df, ki, &kb) || dyn_df_bind(ctx, df, vi, &vb))
        goto fail;
    if (vb.type == DF_STR) {
        JS_ThrowTypeError(ctx, "cannot sum a string column");
        goto fail;
    }

    {
        dfk_acc_t a = { acc, NULL, NULL, NULL };
        dfk_group_scan(kb.type, kb.p, vb.type, vb.p, kb.n < vb.n ? kb.n : vb.n, mask, ngroups,
            DFK_AGG_SUM, &a);
    }
    vals = df_to_typed_array(ctx, acc, (size_t)nkeys * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
    acc = NULL;
    if (JS_IsException(vals))
        goto fail;

    keys = JS_NewArray(ctx);
    if (JS_IsException(keys))
        goto fail;
    for (i = 0; i < nkeys; i++) {
        JSValue k;
        if (df->cols[ki].type == DF_STR)
            k = JS_NewString(ctx, df->cols[ki].dict[i]);
        else
            k = JS_NewInt64(ctx, i);
        if (JS_IsException(k) || JS_DefinePropertyValueUint32(ctx, keys, i, k, JS_PROP_C_W_E) < 0)
            goto fail;
    }

    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        goto fail;
    if (JS_DefinePropertyValueStr(ctx, res, "keys", keys, JS_PROP_C_W_E) < 0) {
        keys = JS_UNDEFINED;
        goto fail;
    }
    keys = JS_UNDEFINED;
    if (JS_DefinePropertyValueStr(ctx, res, "values", vals, JS_PROP_C_W_E) < 0) {
        vals = JS_UNDEFINED;
        goto fail;
    }
    free(acc);
    return res;

fail:
    free(acc);
    JS_FreeValue(ctx, keys);
    JS_FreeValue(ctx, vals);
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}

JSValue dyn_df_group_array_dense(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DfcGrouped G;
    const uint8_t* mask;
    int ki, vi, ok;
    JSValue keys, values;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ki = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ki < 0)
        return JS_EXCEPTION;
    vi = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (vi < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dfc_group_gather(ctx, df, ki, vi, mask, magic == DFC_GROUP_UNIQ, &G))
        return JS_EXCEPTION;

    values = dfc_values_array(ctx, &G);
    keys = JS_IsException(values) ? JS_EXCEPTION
                                  : dfc_dense_keys(ctx, df, ki, G.nkeys);
    dfc_grouped_free(&G);
    if (JS_IsException(values) || JS_IsException(keys)) {
        JS_FreeValue(ctx, values);
        JS_FreeValue(ctx, keys);
        return JS_EXCEPTION;
    }
    return dfc_pair(ctx, keys, values);
}

JSValue dyn_df_group_array_moving_dense(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DfcGrouped G;
    const uint8_t* mask;
    const char* op = magic == DFC_MOVING_AVG ? "GROUP_ARRAY_MOVING_AVG"
                                             : "GROUP_ARRAY_MOVING_SUM";
    double wd = 0.0;
    uint32_t g, w;
    int ki, vi, ok;
    JSValue keys, values;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ki = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ki < 0)
        return JS_EXCEPTION;
    vi = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (vi < 0)
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        if (JS_ToFloat64(ctx, &wd, argv[2]))
            return JS_EXCEPTION;
        if (!(wd >= 1) || wd != floor(wd) || wd > (double)UINT32_MAX)
            return JS_ThrowRangeError(ctx, "%s: window must be a positive "
                                           "integer, got %g",
                op, wd);
    }
    w = (uint32_t)wd;
    mask = df_mask_arg(ctx, argc > 3 ? argv[3] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dfc_group_gather(ctx, df, ki, vi, mask, 0, &G))
        return JS_EXCEPTION;
    for (g = 0; g < G.nkeys; g++)
        dfc_moving_slice(G.flat + (G.off[g] - G.cnt[g]), G.cnt[g], w,
            magic == DFC_MOVING_AVG);

    values = dfc_values_array(ctx, &G);
    keys = JS_IsException(values) ? JS_EXCEPTION
                                  : dfc_dense_keys(ctx, df, ki, G.nkeys);
    dfc_grouped_free(&G);
    if (JS_IsException(values) || JS_IsException(keys)) {
        JS_FreeValue(ctx, values);
        JS_FreeValue(ctx, keys);
        return JS_EXCEPTION;
    }
    return dfc_pair(ctx, keys, values);
}

JSValue dyn_df_group_agg_dense(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound kb, vb;
    const uint8_t* mask;
    int ki, vi = -1, ok, wants_value = (magic != DF_COUNT);
    uint32_t i, ngroups, nkeys;
    double* acc = NULL;
    uint32_t* cnt = NULL;
    JSValue keys = JS_UNDEFINED, vals = JS_UNDEFINED, res = JS_UNDEFINED;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ki = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ki < 0)
        return JS_EXCEPTION;
    if (wants_value) {
        vi = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
        if (vi < 0)
            return JS_EXCEPTION;
    } else if (argc > 1 && JS_IsString(argv[1])) {
        return JS_ThrowTypeError(ctx, "GROUP_BY_COUNT(keyCol[, mask]) takes no "
                                      "value column");
    }
    mask = df_mask_arg(ctx, argc > (wants_value ? 2 : 1) ? argv[wants_value ? 2 : 1] : JS_UNDEFINED,
        df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dfc_group_count(ctx, df, ki, &nkeys, &ngroups))
        return JS_EXCEPTION;

    acc = calloc(ngroups, sizeof(double));
    cnt = calloc(ngroups, sizeof(uint32_t));
    if (!acc || !cnt) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    if (dyn_df_bind(ctx, df, ki, &kb))
        goto fail;
    if (wants_value) {
        if (dyn_df_bind(ctx, df, vi, &vb))
            goto fail;
        if (vb.type == DF_STR) {
            JS_ThrowTypeError(ctx, "cannot %s a string column",
                dfg_agg_verb(magic));
            goto fail;
        }
    }

    {
        uint32_t n = kb.n;
        unsigned agg = magic == DF_MIN ? DFK_AGG_MIN : magic == DF_MAX ? DFK_AGG_MAX
            : magic == DF_COUNT                                        ? DFK_AGG_COUNT
                                                                       : DFK_AGG_MEAN;
        dfk_acc_t a = { acc, acc, acc, cnt };
        if (wants_value && vb.n < n)
            n = vb.n;
        dfk_group_scan(kb.type, kb.p, wants_value ? vb.type : DF_F64, wants_value ? vb.p : NULL, n, mask,
            ngroups, agg, &a);
        if (magic == DF_SUM) {
            for (i = 0; i < nkeys; i++)
                if (!cnt[i])
                    acc[i] = DYN_NAN;
        } else
            dfk_group_finish(agg, &a, nkeys, acc);
    }
    free(cnt);
    cnt = NULL;

    vals = df_to_typed_array(ctx, acc, (size_t)nkeys * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
    acc = NULL;
    if (JS_IsException(vals))
        goto fail;

    keys = JS_NewArray(ctx);
    if (JS_IsException(keys))
        goto fail;
    for (i = 0; i < nkeys; i++) {
        JSValue k;
        if (df->cols[ki].type == DF_STR)
            k = JS_NewString(ctx, df->cols[ki].dict[i]);
        else
            k = JS_NewInt64(ctx, i);
        if (JS_IsException(k) || JS_DefinePropertyValueUint32(ctx, keys, i, k, JS_PROP_C_W_E) < 0)
            goto fail;
    }

    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        goto fail;
    if (JS_DefinePropertyValueStr(ctx, res, "keys", keys, JS_PROP_C_W_E) < 0) {
        keys = JS_UNDEFINED;
        goto fail;
    }
    keys = JS_UNDEFINED;
    if (JS_DefinePropertyValueStr(ctx, res, "values", vals, JS_PROP_C_W_E) < 0) {
        vals = JS_UNDEFINED;
        goto fail;
    }
    return res;

fail:
    free(acc);
    free(cnt);
    JS_FreeValue(ctx, keys);
    JS_FreeValue(ctx, vals);
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}

JSValue dyn_df_group_bit_dense(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const nm[3] = { "GROUP_BIT_AND", "GROUP_BIT_OR",
        "GROUP_BIT_XOR" };
    DataFrame* df;
    DFBound kb, vb;
    const uint8_t* mask;
    uint32_t *acc = NULL, *cnt = NULL, *gk = NULL;
    double* out = NULL;
    int ki, vi, ok;
    uint32_t i, g, n, nkeys, ngroups;
    JSValue keys = JS_UNDEFINED, vals = JS_UNDEFINED, res;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ki = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ki < 0)
        return JS_EXCEPTION;
    vi = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (vi < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (dfc_group_count(ctx, df, ki, &nkeys, &ngroups))
        return JS_EXCEPTION;
    if (dyn_df_bind(ctx, df, ki, &kb) || dyn_df_bind(ctx, df, vi, &vb))
        return JS_EXCEPTION;
    if (vb.type == DF_STR || vb.type == DF_F64 || vb.type == DF_F32)
        return JS_ThrowTypeError(ctx, "%s: column is %s; a bitwise fold is "
                                      "defined only on integer columns",
            nm[magic],
            df_type_name(vb.type));

    acc = calloc(ngroups, sizeof(*acc));
    cnt = calloc(ngroups, sizeof(*cnt));
    if (!acc || !cnt) {
        free(acc);
        free(cnt);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (magic == DFY_BIT_AND)
        for (i = 0; i < ngroups; i++)
            acc[i] = ~0u;

    n = kb.n < vb.n ? kb.n : vb.n;
    gk = df_keys_u32(&kb, n);
    for (i = 0; i < n; i++) {
        uint32_t v;
        if (mask && !mask[i])
            continue;
        g = gk ? gk[i] : (uint32_t)df_get(kb.p, kb.type, i);
        if (g >= ngroups)
            continue;
        v = (uint32_t)(int64_t)df_get(vb.p, vb.type, i);
        if (magic == DFY_BIT_AND)
            acc[g] &= v;
        else if (magic == DFY_BIT_OR)
            acc[g] |= v;
        else
            acc[g] ^= v;
        cnt[g]++;
    }
    free(gk);

    out = df_out_alloc(ctx, nkeys, sizeof(double));
    if (!out) {
        free(acc);
        free(cnt);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nkeys; i++)
        out[i] = (double)acc[i];
    free(acc);
    free(cnt);

    keys = JS_NewArray(ctx);
    if (JS_IsException(keys)) {
        free(out);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nkeys; i++) {
        JSValue k = (df->cols[ki].type == DF_STR)
            ? JS_NewString(ctx, df->cols[ki].dict[i])
            : JS_NewInt64(ctx, i);
        if (JS_IsException(k) || JS_DefinePropertyValueUint32(ctx, keys, i, k, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, keys);
            free(out);
            return JS_EXCEPTION;
        }
    }
    vals = df_to_typed_array(ctx, out, (size_t)nkeys * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
    if (JS_IsException(vals)) {
        JS_FreeValue(ctx, keys);
        return JS_EXCEPTION;
    }
    (void)res;
    return dfc_pair(ctx, keys, vals);
}

JSValue dyn_df_group_concat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    const char* sep = NULL;
    char* buf = NULL;
    size_t len = 0, cap = 0, seplen;
    JSValue out;
    uint32_t i, span;
    int idx, ok, first = 1;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        sep = JS_ToCString(ctx, argv[1]);
        if (!sep)
            return JS_EXCEPTION;
    }
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok || dyn_df_bind(ctx, df, idx, &b)) {
        if (sep)
            JS_FreeCString(ctx, sep);
        return JS_EXCEPTION;
    }
    seplen = sep ? strlen(sep) : 1;

    span = b.n < df->nrows ? b.n : df->nrows;
    for (i = 0; i < span; i++) {
        char num[40];
        const char* piece;
        size_t plen, need;
        if (mask && !mask[i])
            continue;
        if (b.type == DF_STR) {
            int32_t code = ((const int32_t*)b.p)[i];
            piece = (code >= 0 && (uint32_t)code < df->cols[idx].dict_len)
                ? df->cols[idx].dict[code]
                : "";
            plen = strlen(piece);
        } else {
            double d = df_get_call(b.p, b.type, i);
            if (d == floor(d) && d > -1e15 && d < 1e15) {
                int64_t iv = (int64_t)d;
                char tmp[24];
                int tn = 0, neg = iv < 0;
                uint64_t u = neg ? (uint64_t)(-iv) : (uint64_t)iv;
                do {
                    tmp[tn++] = (char)('0' + (u % 10));
                    u /= 10;
                } while (u);
                plen = 0;
                if (neg)
                    num[plen++] = '-';
                while (tn)
                    num[plen++] = tmp[--tn];
                piece = num;
            } else {
                df_fmt_double(num, sizeof(num), d);
                plen = strlen(num);
                piece = num;
            }
        }
        need = len + plen + (first ? 0 : seplen);
        if (need > cap) {
            char* nb;
            size_t ncap = cap ? cap * 2 : 256;
            while (ncap < need)
                ncap *= 2;
            nb = realloc(buf, ncap);
            if (!nb) {
                free(buf);
                if (sep)
                    JS_FreeCString(ctx, sep);
                return JS_ThrowOutOfMemory(ctx);
            }
            buf = nb;
            cap = ncap;
        }
        if (!first) {
            memcpy(buf + len, sep ? sep : ",", seplen);
            len += seplen;
        }
        first = 0;
        memcpy(buf + len, piece, plen);
        len += plen;
    }
    if (sep)
        JS_FreeCString(ctx, sep);
    out = JS_NewStringLen(ctx, buf ? buf : "", len);
    free(buf);
    return out;
}

static int dfz_cmp_d(const void* pa, const void* pb)
{
    double a = *(const double*)pa, b = *(const double*)pb;

    if (a != a)
        return b != b ? 0 : 1;
    if (b != b)
        return -1;
    if (a != b)
        return a < b ? -1 : 1;
    return (signbit(b) != 0) - (signbit(a) != 0);
}

JSValue dyn_df_group_array_v_dense(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const nm[3] = { "GROUP_ARRAY_SORTED",
        "GROUP_ARRAY_LAST",
        "GROUP_ARRAY_SAMPLE" };
    DataFrame* df;
    DFBound kb, vb;
    const uint8_t* mask;
    uint32_t *cnt = NULL, *off = NULL, *gk = NULL, *fill = NULL, *head = NULL;
    double *flat = NULL, kd = 0.0;
    int ki, vi, ok;
    uint32_t i, g, n, nkeys, ngroups, cap, total = 0;
    JSValue keys = JS_UNDEFINED, values = JS_UNDEFINED;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ki = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ki < 0)
        return JS_EXCEPTION;
    vi = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (vi < 0)
        return JS_EXCEPTION;
    if (magic != DFZ_SORTED) {
        if (JS_ToFloat64(ctx, &kd, argc > 2 ? argv[2] : JS_UNDEFINED))
            return JS_EXCEPTION;
        if (!(kd >= 1.0) || kd != floor(kd) || kd > 65536.0)
            return JS_ThrowRangeError(ctx, "%s(key, val, k): k must be an "
                                           "integer in [1, 65536], got %g",
                nm[magic], kd);
    }
    mask = df_mask_arg(ctx, argc > (magic == DFZ_SORTED ? 2 : 3) ? argv[magic == DFZ_SORTED ? 2 : 3] : JS_UNDEFINED,
        df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (dfc_group_count(ctx, df, ki, &nkeys, &ngroups))
        return JS_EXCEPTION;
    if (dyn_df_bind(ctx, df, ki, &kb) || dyn_df_bind(ctx, df, vi, &vb))
        return JS_EXCEPTION;
    if (vb.type == DF_STR)
        return JS_ThrowTypeError(ctx, "%s: the value column is %s; only numeric "
                                      "columns can be collected",
            nm[magic],
            df_type_name(vb.type));
    cap = magic == DFZ_SORTED ? 0xffffffffu : (uint32_t)kd;

    cnt = calloc(ngroups, sizeof(*cnt));
    off = calloc(ngroups + 1, sizeof(*off));
    fill = calloc(ngroups, sizeof(*fill));
    head = calloc(ngroups, sizeof(*head));
    if (!cnt || !off || !fill || !head) {
        free(cnt);
        free(off);
        free(fill);
        free(head);
        return JS_ThrowOutOfMemory(ctx);
    }
    n = kb.n < vb.n ? kb.n : vb.n;
    gk = df_keys_u32(&kb, n);

    for (i = 0; i < n; i++) {
        if (mask && !mask[i])
            continue;
        g = gk ? gk[i] : (uint32_t)df_get(kb.p, kb.type, i);
        if (g >= ngroups)
            continue;
        cnt[g]++;
    }
    for (i = 0; i < ngroups; i++) {
        if (cnt[i] > cap)
            cnt[i] = cap;
        off[i + 1] = off[i] + cnt[i];
    }
    total = off[ngroups];
    flat = df_out_alloc(ctx, total ? total : 1, sizeof(double));
    if (!flat) {
        free(cnt);
        free(off);
        free(fill);
        free(head);
        free(gk);
        return JS_EXCEPTION;
    }

    for (i = 0; i < n; i++) {
        double v;
        uint32_t base, have;
        if (mask && !mask[i])
            continue;
        g = gk ? gk[i] : (uint32_t)df_get(kb.p, kb.type, i);
        if (g >= ngroups)
            continue;
        v = df_get(vb.p, vb.type, i);
        base = off[g];
        have = fill[g];
        if (magic == DFZ_LAST && have == cnt[g] && cnt[g]) {
            flat[base + head[g]] = v;
            {
                uint32_t h = head[g] + 1;
                head[g] = h == cnt[g] ? 0 : h;
            }
            continue;
        }
        if (have < cnt[g]) {
            flat[base + have] = v;
            fill[g] = have + 1;
        }
    }
    free(gk);
    if (magic == DFZ_SORTED)
        for (g = 0; g < nkeys; g++)
            if (cnt[g] > 1 && dfo_f64_sort(&flat[off[g]], cnt[g]))
                qsort(&flat[off[g]], cnt[g], sizeof(double), dfz_cmp_d);

    values = JS_NewArray(ctx);
    if (JS_IsException(values)) {
        free(cnt);
        free(off);
        free(fill);
        free(head);
        free(flat);
        return JS_EXCEPTION;
    }
    for (g = 0; g < nkeys; g++) {
        double* chunk = df_out_alloc(ctx, cnt[g] ? cnt[g] : 1, sizeof(double));
        JSValue ta;
        if (!chunk) {
            JS_FreeValue(ctx, values);
            free(cnt);
            free(off);
            free(fill);
            free(head);
            free(flat);
            return JS_EXCEPTION;
        }
        if (magic == DFZ_LAST && head[g]) {
            uint32_t j, idx = head[g];
            for (j = 0; j < cnt[g]; j++) {
                chunk[j] = flat[off[g] + idx];
                if (++idx == cnt[g])
                    idx = 0;
            }
        } else {
            memcpy(chunk, &flat[off[g]], (size_t)cnt[g] * sizeof(double));
        }
        ta = df_to_typed_array(ctx, chunk, (size_t)cnt[g] * sizeof(double),
            JS_TYPED_ARRAY_FLOAT64);
        if (JS_IsException(ta) || JS_DefinePropertyValueUint32(ctx, values, g, ta, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, values);
            free(cnt);
            free(off);
            free(fill);
            free(head);
            free(flat);
            return JS_EXCEPTION;
        }
    }
    free(cnt);
    free(off);
    free(fill);
    free(head);
    free(flat);

    keys = JS_NewArray(ctx);
    if (JS_IsException(keys)) {
        JS_FreeValue(ctx, values);
        return JS_EXCEPTION;
    }
    for (g = 0; g < nkeys; g++) {
        JSValue k = (df->cols[ki].type == DF_STR)
            ? JS_NewString(ctx, df->cols[ki].dict[g])
            : JS_NewInt64(ctx, g);
        if (JS_IsException(k) || JS_DefinePropertyValueUint32(ctx, keys, g, k, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, keys);
            JS_FreeValue(ctx, values);
            return JS_EXCEPTION;
        }
    }
    return dfc_pair(ctx, keys, values);
}

JSValue dyn_df_group_intersect(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound kb, vb;
    const uint8_t* mask;
    DfcSet vals = { 0 }, pairs = { 0 };
    uint32_t *gk = NULL, *counts = NULL;
    uint8_t* active = NULL;
    double* out = NULL;
    int ki, vi, ok;
    uint32_t i, g, n, nkeys, ngroups, nvals, nout = 0, nactive = 0;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ki = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ki < 0)
        return JS_EXCEPTION;
    vi = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (vi < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (dfc_group_count(ctx, df, ki, &nkeys, &ngroups))
        return JS_EXCEPTION;
    if (dyn_df_bind(ctx, df, ki, &kb))
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, vi, &vb, "GROUP_ARRAY_INTERSECT"))
        return JS_EXCEPTION;

    n = kb.n < vb.n ? kb.n : vb.n;
    counts = calloc(n ? n : 1, sizeof(*counts));
    active = calloc(ngroups ? ngroups : 1, 1);
    if (!counts || !active) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    gk = df_keys_u32(&kb, n);
    for (i = 0; i < n; i++) {
        double v;
        uint32_t e, e2;
        int fresh;
        if (mask && !mask[i])
            continue;
        g = gk ? gk[i] : (uint32_t)df_get(kb.p, kb.type, i);
        if (g >= ngroups)
            continue;
        active[g] = 1;
        v = df_get(vb.p, vb.type, i);
        if (v != v)
            continue;
        if (dfc_set_put(ctx, &vals, dfc_key(v), 0, &e, NULL))
            goto fail;
        if (dfc_set_put(ctx, &pairs, dfc_key(v), g, &e2, &fresh))
            goto fail;
        if (fresh)
            counts[e]++;
    }
    for (g = 0; g < nkeys; g++)
        nactive += active[g];
    nvals = vals.nent;
    out = df_out_alloc(ctx, nvals ? nvals : 1, sizeof(double));
    if (!out)
        goto fail;
    if (nactive)
        for (i = 0; i < nvals; i++)
            if (counts[i] == nactive)
                out[nout++] = dfc_key_value(vals.keys[i]);
    if (nout > 1)
        qsort(out, nout, sizeof(*out), dfz_cmp_d);
    dfc_set_free(&vals);
    dfc_set_free(&pairs);
    free(gk);
    free(counts);
    free(active);
    return df_to_typed_array(ctx, out, (size_t)nout * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
fail:
    dfc_set_free(&vals);
    dfc_set_free(&pairs);
    free(gk);
    free(counts);
    free(active);
    free(out);
    return JS_EXCEPTION;
}

JSValue dyn_df_group_insert_at(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound bv, bp;
    const uint8_t* mask;
    double *out, sz = 0, fill = 0;
    uint32_t i, span, size;
    int iv, ip, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    iv = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (iv < 0)
        return JS_EXCEPTION;
    ip = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (ip < 0)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &sz, argc > 2 ? argv[2] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (argc > 3 && !JS_IsUndefined(argv[3]) && JS_ToFloat64(ctx, &fill, argv[3]))
        return JS_EXCEPTION;
    if (!(sz >= 0.0) || sz != floor(sz) || sz > (double)DF_MAX_GROUPS)
        return JS_ThrowRangeError(ctx, "GROUP_ARRAY_INSERT_AT(value, position, "
                                       "size): size must be an integer in [0, %d], "
                                       "got %g",
            DF_MAX_GROUPS, sz);
    mask = df_mask_arg(ctx, argc > 4 ? argv[4] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, iv, &bv, "GROUP_ARRAY_INSERT_AT") || df_bind_numeric(ctx, df, ip, &bp, "GROUP_ARRAY_INSERT_AT"))
        return JS_EXCEPTION;

    size = (uint32_t)sz;
    out = df_out_alloc(ctx, size ? size : 1, sizeof(double));
    if (!out)
        return JS_EXCEPTION;
    for (i = 0; i < size; i++)
        out[i] = fill;
    span = bv.n < bp.n ? bv.n : bp.n;
    if (span > df->nrows)
        span = df->nrows;
    for (i = 0; i < span; i++) {
        double pos;
        if (mask && !mask[i])
            continue;
        pos = df_get(bp.p, bp.type, i);
        if (!(pos >= 0.0) || pos != floor(pos) || pos >= (double)size)
            continue;
        out[(uint32_t)pos] = df_get(bv.p, bv.type, i);
    }
    return df_to_typed_array(ctx, out, (size_t)size * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

JSValue dyn_df_group_bitmap(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    uint32_t* bits = NULL;
    uint32_t i, span, nw, count = 0;
    double mx = -1.0;
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
    if (dyn_df_bind(ctx, df, idx, &b))
        return JS_EXCEPTION;
    if (b.type == DF_STR || b.type == DF_F64 || b.type == DF_F32)
        return JS_ThrowTypeError(ctx, "GROUP_BITMAP: column '%s' is %s; a "
                                      "bitmap is defined on integer columns",
            df->cols[idx].name, df_type_name(b.type));

    span = b.n < df->nrows ? b.n : df->nrows;
    for (i = 0; i < span; i++) {
        double v;
        if (mask && !mask[i])
            continue;
        v = df_get(b.p, b.type, i);
        if (v < 0.0)
            return JS_ThrowRangeError(ctx, "GROUP_BITMAP: negative value %g; a "
                                           "bitmap indexes by the value itself",
                v);
        if (v > mx)
            mx = v;
    }
    if (mx < 0.0)
        return JS_NewUint32(ctx, 0);
    if (mx >= (double)DF_BITMAP_MAX_BITS)
        return JS_ThrowRangeError(ctx, "GROUP_BITMAP: value %g exceeds the "
                                       "bitmap range of %u; use N_UNIQUE, which is "
                                       "bounded by the row count",
            mx,
            DF_BITMAP_MAX_BITS);
    nw = ((uint32_t)mx >> 5) + 1;
    bits = calloc(nw, sizeof(*bits));
    if (!bits)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < span; i++) {
        uint32_t v, w, bit;
        if (mask && !mask[i])
            continue;
        v = (uint32_t)df_get(b.p, b.type, i);
        w = v >> 5;
        bit = 1u << (v & 31);
        count += (bits[w] & bit) == 0;
        bits[w] |= bit;
    }
    free(bits);
    return JS_NewUint32(ctx, count);
}

static JSValue dyn_df_group_by_sum_m(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    (void)magic;
    return dyn_df_group_by_sum_dense(ctx, this_val, argc, argv);
}

JSValue dyn_df_group_by_sum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dfc_grouped(ctx, this_val, argc, argv, 0, dyn_df_group_by_sum_m);
}

static const char* const dfg_agg_names[5] = { "sum", "mean", "count", "min", "max" };
static const unsigned dfg_agg_bits[5] = { DFK_AGG_SUM, DFK_AGG_MEAN, DFK_AGG_COUNT, DFK_AGG_MIN, DFK_AGG_MAX };

static int dfg_agg_list(JSContext* ctx, JSValueConst v, unsigned* paggs)
{
    int64_t len;
    uint32_t i;
    unsigned aggs = 0;
    JSValue lv;
    if (JS_IsArray(ctx, v) != 1) {
        JS_ThrowTypeError(ctx, "GROUP_BY_AGG(key, val, aggs[, mask]): aggs must be an array of aggregate names");
        return -1;
    }
    lv = JS_GetPropertyStr(ctx, v, "length");
    if (JS_IsException(lv) || JS_ToInt64(ctx, &len, lv)) {
        JS_FreeValue(ctx, lv);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    if (len == 0) {
        JS_ThrowTypeError(ctx, "GROUP_BY_AGG: aggs is empty (name at least one of sum, mean, count, min, max)");
        return -1;
    }
    if (len > 64) {
        JS_ThrowTypeError(ctx, "GROUP_BY_AGG: aggs lists %lld names; each of sum, mean, count, min, max may appear once", (long long)len);
        return -1;
    }
    for (i = 0; i < (uint32_t)len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, i);
        const char* name;
        int k;
        if (JS_IsException(e))
            return -1;
        if (!JS_IsString(e)) {
            JS_FreeValue(ctx, e);
            JS_ThrowTypeError(ctx, "GROUP_BY_AGG: aggs[%u] is not a string", i);
            return -1;
        }
        name = JS_ToCString(ctx, e);
        JS_FreeValue(ctx, e);
        if (!name)
            return -1;
        for (k = 0; k < 5; k++)
            if (strcmp(name, dfg_agg_names[k]) == 0)
                break;
        if (k == 5) {
            JS_ThrowTypeError(ctx, "GROUP_BY_AGG: unknown aggregate '%s' (one of sum, mean, count, min, max)", name);
            JS_FreeCString(ctx, name);
            return -1;
        }
        if (aggs & dfg_agg_bits[k]) {
            JS_ThrowTypeError(ctx, "GROUP_BY_AGG: aggregate '%s' is listed twice", name);
            JS_FreeCString(ctx, name);
            return -1;
        }
        JS_FreeCString(ctx, name);
        aggs |= dfg_agg_bits[k];
    }
    *paggs = aggs;
    return 0;
}

JSValue dyn_df_group_by_agg(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound kb, vb;
    const uint8_t* mask;
    int ki, vi, ok, k;
    unsigned aggs = 0;
    uint32_t i, j, ngroups, nkeys, have;
    dfk_acc_t a = { NULL, NULL, NULL, NULL };
    uint8_t* seen = NULL;
    double* tmp = NULL;
    JSValue key, keys = JS_UNDEFINED, res = JS_UNDEFINED;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    key = JS_ToString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(key))
        return key;
    ki = df_col_arg(ctx, df, key);
    JS_FreeValue(ctx, key);
    if (ki < 0)
        return JS_EXCEPTION;
    vi = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (vi < 0)
        return JS_EXCEPTION;
    if (dfg_agg_list(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, &aggs))
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 3 ? argv[3] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (dfc_group_count(ctx, df, ki, &nkeys, &ngroups))
        return JS_EXCEPTION;
    if (dyn_df_bind(ctx, df, ki, &kb) || dyn_df_bind(ctx, df, vi, &vb))
        return JS_EXCEPTION;
    if (vb.type == DF_STR)
        return JS_ThrowTypeError(ctx, "GROUP_BY_AGG: cannot aggregate a string column");

    if (aggs & (DFK_AGG_SUM | DFK_AGG_MEAN))
        a.sum = malloc((size_t)ngroups * sizeof(double));
    if (aggs & DFK_AGG_MIN)
        a.min = malloc((size_t)ngroups * sizeof(double));
    if (aggs & DFK_AGG_MAX)
        a.max = malloc((size_t)ngroups * sizeof(double));
    if (aggs & (DFK_AGG_COUNT | DFK_AGG_MEAN | DFK_AGG_MIN | DFK_AGG_MAX))
        a.cnt = malloc((size_t)ngroups * sizeof(uint32_t));
    seen = malloc((size_t)ngroups);
    tmp = malloc((size_t)ngroups * sizeof(double));
    if (!seen || !tmp || ((aggs & (DFK_AGG_SUM | DFK_AGG_MEAN)) && !a.sum) || ((aggs & DFK_AGG_MIN) && !a.min)
        || ((aggs & DFK_AGG_MAX) && !a.max) || ((aggs & ~(unsigned)DFK_AGG_SUM) && !a.cnt)) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    dfk_group_scan(kb.type, kb.p, vb.type, vb.p, kb.n < vb.n ? kb.n : vb.n, mask, ngroups, aggs, &a);

    if (df->cols[ki].type == DF_STR)
        have = dfk_keys_present(kb.p, kb.n, nkeys, seen);
    else {
        memset(seen, 1, ngroups);
        have = nkeys;
    }

    res = JS_NewObject(ctx);
    keys = JS_NewArray(ctx);
    if (JS_IsException(res) || JS_IsException(keys))
        goto fail;
    for (i = 0, j = 0; i < nkeys; i++) {
        JSValue kv;
        if (!seen[i])
            continue;
        if (df->cols[ki].type == DF_STR)
            kv = JS_NewString(ctx, df->cols[ki].dict[i]);
        else
            kv = JS_NewInt64(ctx, i);
        if (JS_IsException(kv) || JS_DefinePropertyValueUint32(ctx, keys, j++, kv, JS_PROP_C_W_E) < 0)
            goto fail;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "keys", keys, JS_PROP_C_W_E) < 0) {
        keys = JS_UNDEFINED;
        goto fail;
    }
    keys = JS_UNDEFINED;
    for (k = 0; k < 5; k++) {
        double* out;
        JSValue col;
        if (!(aggs & dfg_agg_bits[k]))
            continue;
        out = malloc((have ? have : 1) * sizeof(double));
        if (!out) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        dfk_group_finish(dfg_agg_bits[k], &a, nkeys, tmp);
        for (i = 0, j = 0; i < nkeys; i++)
            if (seen[i])
                out[j++] = tmp[i];
        col = df_to_typed_array(ctx, out, (size_t)have * sizeof(double), JS_TYPED_ARRAY_FLOAT64);
        if (JS_IsException(col) || JS_DefinePropertyValueStr(ctx, res, dfg_agg_names[k], col, JS_PROP_C_W_E) < 0)
            goto fail;
    }
    free(a.sum);
    free(a.min);
    free(a.max);
    free(a.cnt);
    free(seen);
    free(tmp);
    return res;

fail:
    free(a.sum);
    free(a.min);
    free(a.max);
    free(a.cnt);
    free(seen);
    free(tmp);
    JS_FreeValue(ctx, keys);
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}

#endif
