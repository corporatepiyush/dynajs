#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

enum { DFJ_INNER,
    DFJ_LEFT,
    DFJ_RIGHT,
    DFJ_OUTER };

#define DFJ_OUT_BUDGET ((uint64_t)1024 * 1024 * 1024)

typedef struct {
    uint64_t* keys;
    uint32_t* heads;
    uint32_t* next;
    uint32_t* knext;
    uint32_t* tail;
    uint32_t* row;
    uint32_t* len;
    uint32_t nslots, mask, nent, cap;
} DfJoinIndex;

static void dfj_free(DfJoinIndex* j)
{
    free(j->keys);
    free(j->heads);
    free(j->next);
    free(j->knext);
    free(j->tail);
    free(j->row);
    free(j->len);
    memset(j, 0, sizeof(*j));
}

static int dfj_put(JSContext* ctx, DfJoinIndex* j, uint64_t key, uint32_t row)
{
    uint32_t p, e;

    if (j->nent == j->cap) {
        uint32_t ncap = j->cap ? j->cap << 1 : 64;
        uint64_t* nk = realloc(j->keys, (size_t)ncap * sizeof(*nk));
        uint32_t* nn = realloc(j->next, (size_t)ncap * sizeof(*nn));
        uint32_t* nk2 = realloc(j->knext, (size_t)ncap * sizeof(*nk2));
        uint32_t* nt = realloc(j->tail, (size_t)ncap * sizeof(*nt));
        uint32_t* nr = realloc(j->row, (size_t)ncap * sizeof(*nr));
        uint32_t* nl = realloc(j->len, (size_t)ncap * sizeof(*nl));
        if (nk)
            j->keys = nk;
        if (nn)
            j->next = nn;
        if (nk2)
            j->knext = nk2;
        if (nt)
            j->tail = nt;
        if (nr)
            j->row = nr;
        if (nl)
            j->len = nl;
        if (!nk || !nn || !nk2 || !nt || !nr || !nl) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        j->cap = ncap;
    }
    if (j->nent * 2 >= j->nslots) {
        uint32_t nslots = j->nslots ? j->nslots << 1 : 64, i;
        uint32_t* heads = malloc((size_t)nslots * sizeof(*heads));
        if (!heads) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memset(heads, 0xFF, (size_t)nslots * sizeof(*heads));
        for (i = j->nent; i-- > 0;) {
            uint32_t q = dfc_hash(j->keys[i], 0) & (nslots - 1);
            j->next[i] = (heads[q] == 0xFFFFFFFFu) ? 0xFFFFFFFFu : heads[q];
            heads[q] = i;
        }
        free(j->heads);
        j->heads = heads;
        j->nslots = nslots;
        j->mask = nslots - 1;
    }
    p = dfc_hash(key, 0) & j->mask;
    e = j->heads[p];
    while (e != 0xFFFFFFFFu) {
        if (j->keys[e] == key) {
            uint32_t last = j->tail[e];
            j->knext[last] = j->nent;
            j->tail[e] = j->nent;
            j->knext[j->nent] = 0xFFFFFFFFu;
            j->row[j->nent] = row;
            j->keys[j->nent] = key;
            j->len[e]++;
            j->nent++;
            return 0;
        }
        e = j->next[e];
    }
    j->keys[j->nent] = key;
    j->next[j->nent] = j->heads[p];
    j->knext[j->nent] = 0xFFFFFFFFu;
    j->tail[j->nent] = j->nent;
    j->row[j->nent] = row;
    j->len[j->nent] = 1;
    j->heads[p] = j->nent;
    j->nent++;
    return 0;
}

static uint32_t dfj_find(const DfJoinIndex* j, uint64_t key)
{
    uint32_t p, e;

    if (!j->heads)
        return 0xFFFFFFFFu;
    p = dfc_hash(key, 0) & j->mask;
    e = j->heads[p];
    while (e != 0xFFFFFFFFu) {
        if (j->keys[e] == key)
            return e;
        e = j->next[e];
    }
    return 0xFFFFFFFFu;
}

static int dfj_key_col(JSContext* ctx, const DataFrame* df, int idx,
    const char* op, DFBound* b)
{
    if (dyn_df_bind(ctx, df, idx, b))
        return -1;
    if (b->type == DF_STR || b->type == DF_F64 || b->type == DF_F32) {
        JS_ThrowTypeError(ctx, "%s: key column '%s' is %s; a join key must be "
                               "an integer column",
            op, df->cols[idx].name, df_type_name(b->type));
        return -1;
    }
    return 0;
}

JSValue dyn_df_join(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* op = "JOIN";
    DataFrame *df, *other;
    DFBound bl, br;
    DfJoinIndex ridx = { 0 }, lidx = { 0 };
    uint32_t *lrows = NULL, *rrows = NULL, *matched = NULL;
    uint64_t nout = 0, rowcap;
    uint32_t i, lr, rr, e;
    int li2, ri2;
    int how = DFJ_INNER;
    DFBuilder B = { 0 };
    JSValue res;
    uint32_t ncols, c;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    other = dyn_plain_get(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
        dyn_df_class_id);
    if (!other)
        return JS_EXCEPTION;
    li2 = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (li2 < 0)
        return JS_EXCEPTION;
    ri2 = df_col_arg(ctx, other, argc > 2 ? argv[2] : JS_UNDEFINED);
    if (ri2 < 0)
        return JS_EXCEPTION;
    if (argc > 3 && !JS_IsUndefined(argv[3])) {
        const char* hows = JS_ToCString(ctx, argv[3]);
        if (!hows)
            return JS_EXCEPTION;
        if (!strcmp(hows, "inner"))
            how = DFJ_INNER;
        else if (!strcmp(hows, "left"))
            how = DFJ_LEFT;
        else if (!strcmp(hows, "right"))
            how = DFJ_RIGHT;
        else if (!strcmp(hows, "outer"))
            how = DFJ_OUTER;
        else {
            JS_FreeCString(ctx, hows);
            return JS_ThrowRangeError(ctx, "%s(..., how): how must be one of "
                                           "inner/left/right/outer",
                op);
        }
        JS_FreeCString(ctx, hows);
    }
    if (dfj_key_col(ctx, df, (int)li2, op, &bl))
        return JS_EXCEPTION;
    if (dfj_key_col(ctx, other, (int)ri2, op, &br))
        return JS_EXCEPTION;

    for (rr = 0; rr < br.n; rr++)
        if (dfj_put(ctx, &ridx, dfc_key(df_get(br.p, br.type, rr)), rr))
            goto fail;
    if (how == DFJ_RIGHT || how == DFJ_OUTER)
        for (lr = 0; lr < bl.n; lr++)
            if (dfj_put(ctx, &lidx, dfc_key(df_get(bl.p, bl.type, lr)), lr))
                goto fail;
    if (how == DFJ_OUTER) {
        matched = calloc(br.n ? br.n : 1, sizeof(uint32_t));
        if (!matched) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
    }

    ncols = df->ncols + other->ncols + (how == DFJ_OUTER ? 1 : 0);
    rowcap = DFJ_OUT_BUDGET / (8 + (uint64_t)ncols * 8);
    if (rowcap > (uint64_t)DFB_MAX_ROWS)
        rowcap = (uint64_t)DFB_MAX_ROWS;
    {
        uint64_t cap = rowcap;
        nout = 0;
        if (how == DFJ_INNER || how == DFJ_LEFT || how == DFJ_OUTER) {
            for (lr = 0; lr < bl.n; lr++) {
                e = dfj_find(&ridx, dfc_key(df_get(bl.p, bl.type, lr)));
                nout += (e == 0xFFFFFFFFu) ? (uint32_t)(how != DFJ_INNER)
                                           : ridx.len[e];
                if (nout > cap)
                    goto too_many;
            }
        }
        if (how == DFJ_OUTER) {
            for (rr = 0; rr < br.n; rr++)
                if (dfj_find(&lidx, dfc_key(df_get(br.p, br.type, rr)))
                    == 0xFFFFFFFFu) {
                    nout++;
                    if (nout > cap)
                        goto too_many;
                }
        }
        if (how == DFJ_RIGHT) {
            for (rr = 0; rr < br.n; rr++) {
                e = dfj_find(&lidx, dfc_key(df_get(br.p, br.type, rr)));
                nout += (e == 0xFFFFFFFFu) ? 1u : lidx.len[e];
                if (nout > cap)
                    goto too_many;
            }
        }
    }
    goto counted;
too_many:
    JS_ThrowRangeError(ctx, "%s: result would have more than %llu rows "
                            "(%llu columns, about %llu MB); the join key has too "
                            "many matches",
        op, (unsigned long long)rowcap,
        (unsigned long long)ncols,
        (unsigned long long)(DFJ_OUT_BUDGET / (1024 * 1024)));
    goto fail;
counted:;
    lrows = df_out_alloc(ctx, (uint32_t)(nout ? nout : 1), sizeof(uint32_t));
    rrows = df_out_alloc(ctx, (uint32_t)(nout ? nout : 1), sizeof(uint32_t));
    if (!lrows || !rrows)
        goto fail;

    i = 0;
    if (how == DFJ_INNER || how == DFJ_LEFT || how == DFJ_OUTER) {
        for (lr = 0; lr < bl.n; lr++) {
            uint64_t k = dfc_key(df_get(bl.p, bl.type, lr));
            e = dfj_find(&ridx, k);
            if (e == 0xFFFFFFFFu) {
                if (how != DFJ_INNER) {
                    lrows[i] = lr;
                    rrows[i] = DFB_NO_ROW;
                    i++;
                }
            } else {
                for (; e != 0xFFFFFFFFu; e = ridx.knext[e]) {
                    lrows[i] = lr;
                    rrows[i] = ridx.row[e];
                    if (matched)
                        matched[ridx.row[e]] = 1;
                    i++;
                }
            }
        }
    }
    if (how == DFJ_OUTER) {
        for (rr = 0; rr < br.n; rr++)
            if (!matched[rr]) {
                lrows[i] = DFB_NO_ROW;
                rrows[i] = rr;
                i++;
            }
    }
    if (how == DFJ_RIGHT) {
        for (rr = 0; rr < br.n; rr++) {
            uint64_t k = dfc_key(df_get(br.p, br.type, rr));
            e = dfj_find(&lidx, k);
            if (e == 0xFFFFFFFFu) {
                lrows[i] = DFB_NO_ROW;
                rrows[i] = rr;
                i++;
            } else {
                for (; e != 0xFFFFFFFFu; e = lidx.knext[e]) {
                    lrows[i] = lidx.row[e];
                    rrows[i] = rr;
                    i++;
                }
            }
        }
    }
    (void)i;

    if (dfb_init(ctx, &B, op, ncols, nout))
        goto fail;
    {
        int left_miss = (how == DFJ_RIGHT || how == DFJ_OUTER);
        int right_miss = (how == DFJ_LEFT || how == DFJ_OUTER);
        for (c = 0; c < df->ncols; c++) {
            DFType ot = df->cols[c].type;
            if (left_miss) {
                if (ot == DF_STR) {
                    JS_ThrowRangeError(ctx, "%s: left column '%s' is a string "
                                            "column and a %s join can leave a row "
                                            "without a left value",
                        op, df->cols[c].name,
                        how == DFJ_OUTER ? "outer" : "right");
                    goto fail;
                }
                ot = DF_F64;
            }
            if (dfb_add_gather(ctx, &B, df->cols[c].name, df, (int)c, ot,
                    lrows, (uint32_t)nout))
                goto fail;
        }
        for (c = 0; c < other->ncols; c++) {
            char name[64];
            const char* nm = other->cols[c].name;
            DFType ot = other->cols[c].type;
            if (df_find_col(df, nm) >= 0) {
                int nw = snprintf(name, sizeof(name), "%s_right", nm);
                if (nw < 0 || (size_t)nw >= sizeof(name)) {
                    JS_ThrowRangeError(ctx, "%s: renamed column '%s_right' "
                                            "exceeds the %d character name limit",
                        op, nm, (int)sizeof(name) - 1);
                    goto fail;
                }
                if (df_find_col(df, name) >= 0) {
                    JS_ThrowRangeError(ctx, "%s: right column '%s' collides "
                                            "with the carried column '%s_right'",
                        op, nm, nm);
                    goto fail;
                }
                nm = name;
            }
            if (right_miss) {
                if (ot == DF_STR) {
                    JS_ThrowRangeError(ctx, "%s: right column '%s' is a string "
                                            "column and a %s join can leave a row "
                                            "without a right value",
                        op, other->cols[c].name,
                        how == DFJ_OUTER ? "outer" : "left");
                    goto fail;
                }
                ot = DF_F64;
            }
            if (dfb_add_gather(ctx, &B, nm, other, (int)c, ot, rrows,
                    (uint32_t)nout))
                goto fail;
        }
        if (how == DFJ_OUTER) {
            uint8_t* m = df_out_alloc(ctx, (uint32_t)(nout ? nout : 1), 1);
            uint32_t k;
            if (!m)
                goto fail;
            for (k = 0; k < nout; k++)
                m[k] = (rrows[k] != DFB_NO_ROW) ? 1 : 0;
            if (dfb_add(ctx, &B, "matched", DF_U8, m, (uint32_t)nout)) {
                m = NULL;
                goto fail;
            }
        }
    }
    free(lrows);
    free(rrows);
    free(matched);
    dfj_free(&ridx);
    dfj_free(&lidx);
    res = dfb_seal(ctx, &B);
    return res;
fail:
    free(lrows);
    free(rrows);
    free(matched);
    dfj_free(&ridx);
    dfj_free(&lidx);
    dfb_free(ctx, &B);
    return JS_EXCEPTION;
}

JSValue dyn_df_asof_join(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* op = "ASOF_JOIN";
    DataFrame *df, *other;
    DFBound bl, br;
    uint32_t *lrows = NULL, *rrows = NULL;
    uint32_t n, i, lr, rr, rptr = 0;
    uint32_t c;
    int li, ri;
    DFBuilder B = { 0 };
    JSValue res;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    other = dyn_plain_get(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
        dyn_df_class_id);
    if (!other)
        return JS_EXCEPTION;
    li = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (li < 0)
        return JS_EXCEPTION;
    ri = df_col_arg(ctx, other, argc > 2 ? argv[2] : JS_UNDEFINED);
    if (ri < 0)
        return JS_EXCEPTION;
    if (dfj_key_col(ctx, df, li, op, &bl))
        return JS_EXCEPTION;
    if (dfj_key_col(ctx, other, ri, op, &br))
        return JS_EXCEPTION;

    for (i = 1; i < bl.n; i++)
        if (df_get(bl.p, bl.type, i) < df_get(bl.p, bl.type, i - 1))
            return JS_ThrowRangeError(ctx, "%s: left time column must be sorted "
                                           "ascending",
                op);
    for (i = 1; i < br.n; i++)
        if (df_get(br.p, br.type, i) < df_get(br.p, br.type, i - 1))
            return JS_ThrowRangeError(ctx, "%s: right time column must be "
                                           "sorted ascending",
                op);
    for (i = 0; i < bl.n; i++)
        if (df_get(bl.p, bl.type, i) != df_get(bl.p, bl.type, i))
            return JS_ThrowRangeError(ctx, "%s: left time column must not "
                                           "contain NaN",
                op);
    for (i = 0; i < br.n; i++)
        if (df_get(br.p, br.type, i) != df_get(br.p, br.type, i))
            return JS_ThrowRangeError(ctx, "%s: right time column must not "
                                           "contain NaN",
                op);

    n = bl.n;
    lrows = df_out_alloc(ctx, n ? n : 1, sizeof(uint32_t));
    rrows = df_out_alloc(ctx, n ? n : 1, sizeof(uint32_t));
    if (!lrows || !rrows)
        goto fail;

    for (lr = 0; lr < n; lr++) {
        double lt = df_get(bl.p, bl.type, lr);
        while (rptr + 1 < br.n && df_get(br.p, br.type, rptr + 1) <= lt)
            rptr++;
        lrows[lr] = lr;
        rrows[lr] = (br.n && df_get(br.p, br.type, rptr) <= lt) ? rptr
                                                                : DFB_NO_ROW;
    }
    (void)rr;

    {
        uint32_t ncols = df->ncols + other->ncols;
        if (dfb_init(ctx, &B, op, ncols, n))
            goto fail;
        for (c = 0; c < df->ncols; c++)
            if (dfb_add_gather(ctx, &B, df->cols[c].name, df, (int)c,
                    df->cols[c].type, lrows, n))
                goto fail;
        for (c = 0; c < other->ncols; c++) {
            char name[64];
            const char* nm = other->cols[c].name;
            DFType ot = other->cols[c].type;
            if (df_find_col(df, nm) >= 0) {
                int nw = snprintf(name, sizeof(name), "%s_right", nm);
                if (nw < 0 || (size_t)nw >= sizeof(name)) {
                    JS_ThrowRangeError(ctx, "%s: renamed column '%s_right' "
                                            "exceeds the %d character name limit",
                        op, nm, (int)sizeof(name) - 1);
                    goto fail;
                }
                nm = name;
            }
            if (ot == DF_STR) {
                JS_ThrowRangeError(ctx, "%s: right column '%s' is a string "
                                        "column and a left row may have no preceding "
                                        "right row",
                    op, other->cols[c].name);
                goto fail;
            }
            ot = DF_F64;
            if (dfb_add_gather(ctx, &B, nm, other, (int)c, ot, rrows, n))
                goto fail;
        }
    }
    free(lrows);
    free(rrows);
    res = dfb_seal(ctx, &B);
    return res;
fail:
    free(lrows);
    free(rrows);
    dfb_free(ctx, &B);
    return JS_EXCEPTION;
}

static JSValue dfja_value(JSContext* ctx, double v, int strict)
{
    if (strict && (v != v || v > 1.0 / 0.0 || v < -1.0 / 0.0))
        return JS_ThrowRangeError(ctx, "JSON_AGG: a non-finite value cannot "
                                       "be represented in JSON");
    return JS_NewFloat64(ctx, v);
}

JSValue dfja_build(JSContext* ctx, const DataFrame* df, int ki,
    const double* keys, const double* vals,
    const uint8_t* mask, uint32_t n, int obj_form,
    int strict)
{
    DfcSet set = { 0 };
    double** grows = NULL;
    uint32_t *gcnt = NULL, *gcap = NULL;
    uint32_t i, g, ng = 0;
    JSValue res, arr;
    int isstr = (df->cols[ki].type == DF_STR);

    for (i = 0; i < n; i++) {
        uint32_t e;
        int fr;
        if (mask && !mask[i])
            continue;
        if (dfc_set_put(ctx, &set, dfc_key(keys[i]), 0, &e, &fr))
            goto fail;
    }
    ng = set.nent;
    if (ng == 0) {
        dfc_set_free(&set);
        return JS_NewObject(ctx);
    }
    gcnt = calloc(ng, sizeof(uint32_t));
    gcap = calloc(ng, sizeof(uint32_t));
    grows = calloc(ng, sizeof(double*));
    if (!gcnt || !gcap || !grows) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    for (i = 0; i < n; i++) {
        uint32_t e;
        if (mask && !mask[i])
            continue;
        if (dfc_set_find(&set, dfc_key(keys[i]), 0, &e))
            continue;
        gcnt[e]++;
    }
    for (g = 0; g < ng; g++) {
        grows[g] = malloc((size_t)(gcnt[g] ? gcnt[g] : 1) * sizeof(double));
        if (!grows[g]) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        gcap[g] = gcnt[g];
        gcnt[g] = 0;
    }
    for (i = 0; i < n; i++) {
        uint32_t e;
        double v = vals[i];
        if (mask && !mask[i])
            continue;
        if (dfc_set_find(&set, dfc_key(keys[i]), 0, &e))
            continue;
        if (strict && (v != v || v > 1.0 / 0.0 || v < -1.0 / 0.0)) {
            JS_ThrowRangeError(ctx, "JSON_AGG: a non-finite value cannot be "
                                    "represented in JSON");
            goto fail;
        }
        grows[e][gcnt[e]++] = v;
    }

    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        goto fail;
    if (obj_form) {
        for (i = 0; i < n; i++) {
            JSValue v;
            char kbuf[64];
            const char* kname;
            double kv = keys[i];
            if (mask && !mask[i])
                continue;
            if (isstr) {
                uint32_t code = (uint32_t)kv;
                if (code >= df->cols[ki].dict_len) {
                    JS_ThrowRangeError(ctx, "JSON_OBJECT_AGG: key code %u out "
                                            "of range",
                        code);
                    JS_FreeValue(ctx, res);
                    goto fail;
                }
                kname = df->cols[ki].dict[code];
            } else {
                df_fmt_double(kbuf, sizeof(kbuf), kv);
                kname = kbuf;
            }
            v = dfja_value(ctx, vals[i], strict);
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, res);
                goto fail;
            }
            if (JS_DefinePropertyValueStr(ctx, res, kname, v, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, res);
                goto fail;
            }
        }
    } else {
        for (g = 0; g < ng; g++) {
            char kbuf[64];
            const char* kname;
            uint64_t bits = set.keys[g];
            double kv = dfc_key_value(bits);
            uint32_t c;
            if (isstr) {
                uint32_t code = (uint32_t)kv;
                if (code >= df->cols[ki].dict_len) {
                    JS_ThrowRangeError(ctx, "JSON_AGG: key code %u out of "
                                            "range",
                        code);
                    JS_FreeValue(ctx, res);
                    goto fail;
                }
                kname = df->cols[ki].dict[code];
            } else {
                df_fmt_double(kbuf, sizeof(kbuf), kv);
                kname = kbuf;
            }
            arr = JS_NewArray(ctx);
            if (JS_IsException(arr)) {
                JS_FreeValue(ctx, res);
                goto fail;
            }
            for (c = 0; c < gcnt[g]; c++) {
                JSValue v = dfja_value(ctx, grows[g][c], strict);
                if (JS_IsException(v)) {
                    JS_FreeValue(ctx, arr);
                    JS_FreeValue(ctx, res);
                    goto fail;
                }
                if (JS_DefinePropertyValueUint32(ctx, arr, c, v,
                        JS_PROP_C_W_E)
                    < 0) {
                    JS_FreeValue(ctx, arr);
                    JS_FreeValue(ctx, res);
                    goto fail;
                }
            }
            if (JS_DefinePropertyValueStr(ctx, res, kname, arr,
                    JS_PROP_C_W_E)
                < 0) {
                JS_FreeValue(ctx, res);
                goto fail;
            }
        }
    }
    dfc_set_free(&set);
    for (g = 0; g < ng; g++)
        free(grows[g]);
    free(grows);
    free(gcnt);
    free(gcap);
    return res;
fail:
    dfc_set_free(&set);
    if (grows) {
        for (g = 0; g < ng; g++)
            free(grows[g]);
        free(grows);
    }
    free(gcnt);
    free(gcap);
    return JS_EXCEPTION;
}

#endif
