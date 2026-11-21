#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

#define DFP_CELL_BUDGET ((uint64_t)256 * 1024 * 1024)

JSValue dyn_df_concat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* op = "CONCAT";
    DataFrame *df, *other;
    DFBuilder B = { 0 };
    uint32_t c, i;
    uint64_t nout;
    JSValue res;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    other = dyn_plain_get(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
        dyn_df_class_id);
    if (!other)
        return JS_EXCEPTION;
    if (df->ncols != other->ncols)
        return JS_ThrowRangeError(ctx, "%s: column count mismatch (%u vs %u)",
            op, df->ncols, other->ncols);
    for (c = 0; c < df->ncols; c++)
        if (strcmp(df->cols[c].name, other->cols[c].name) != 0)
            return JS_ThrowRangeError(ctx, "%s: column %u is '%s' on the left "
                                           "and '%s' on the right; the sets must "
                                           "match in order",
                op, c, df->cols[c].name,
                other->cols[c].name);
    nout = (uint64_t)df->nrows + other->nrows;
    if (dfb_init(ctx, &B, op, df->ncols, nout))
        return JS_EXCEPTION;
    {
        uint32_t n = (uint32_t)nout;

        for (c = 0; c < df->ncols; c++) {
            DFType lt = df->cols[c].type, rt = other->cols[c].type;
            if (lt == DF_STR && rt == DF_STR) {
                DfStrTab tab = { 0 };
                int32_t* codes = df_out_alloc(ctx, n ? n : 1, sizeof(int32_t));
                int32_t* remap;
                uint32_t d;
                DFBound bl, br;
                if (!codes)
                    goto fail;
                if (dyn_df_bind(ctx, df, (int)c, &bl) || dyn_df_bind(ctx, other, (int)c, &br)) {
                    free(codes);
                    goto fail;
                }
                for (d = 0; d < df->cols[c].dict_len; d++) {
                    uint32_t code;
                    if (df_strtab_put(ctx, &tab, df->cols[c].dict[d], &code)) {
                        free(codes);
                        df_strtab_free(&tab);
                        goto fail;
                    }
                }
                remap = malloc((size_t)(other->cols[c].dict_len
                                       ? other->cols[c].dict_len
                                       : 1)
                    * sizeof(int32_t));
                if (!remap) {
                    free(codes);
                    df_strtab_free(&tab);
                    JS_ThrowOutOfMemory(ctx);
                    goto fail;
                }
                for (d = 0; d < other->cols[c].dict_len; d++) {
                    uint32_t code;
                    if (df_strtab_put(ctx, &tab, other->cols[c].dict[d], &code)) {
                        free(codes);
                        free(remap);
                        df_strtab_free(&tab);
                        goto fail;
                    }
                    remap[d] = (int32_t)code;
                }
                for (i = 0; i < df->nrows; i++)
                    codes[i] = ((const int32_t*)bl.p)[i];
                for (i = 0; i < other->nrows; i++)
                    codes[df->nrows + i] = remap[((const int32_t*)br.p)[i]];
                free(remap);
                if (dfb_add_str(ctx, &B, df->cols[c].name, codes, tab.dict,
                        tab.dict_len, n)) {
                    tab.dict = NULL;
                    tab.dict_len = 0;
                    df_strtab_free(&tab);
                    goto fail;
                }
                tab.dict = NULL;
                tab.dict_len = 0;
                df_strtab_free(&tab);
            } else if (lt != DF_STR && rt != DF_STR) {
                DFType outt = (lt == rt) ? lt : DF_F64;
                DFBound bl, br;
                void* out;
                if (dyn_df_bind(ctx, df, (int)c, &bl) || dyn_df_bind(ctx, other, (int)c, &br))
                    goto fail;
                out = df_out_alloc(ctx, n ? n : 1, df_elt_size(outt));
                if (!out)
                    goto fail;
                if (outt == DF_F64) {
                    double* d = out;
                    for (i = 0; i < df->nrows; i++)
                        d[i] = df_get(bl.p, bl.type, i);
                    for (i = 0; i < other->nrows; i++)
                        d[df->nrows + i] = df_get(br.p, br.type, i);
                } else {
                    memcpy(out, bl.p, (size_t)df->nrows * df_elt_size(outt));
                    memcpy((uint8_t*)out + (size_t)df->nrows * df_elt_size(outt),
                        br.p, (size_t)other->nrows * df_elt_size(outt));
                }
                if (dfb_add(ctx, &B, df->cols[c].name, outt, out, n)) {
                    out = NULL;
                    goto fail;
                }
            } else {
                JS_ThrowRangeError(ctx, "%s: column '%s' is %s on the left and %s "
                                        "on the right; a string column cannot meet a "
                                        "numeric one",
                    op, df->cols[c].name,
                    df_type_name(lt), df_type_name(rt));
                goto fail;
            }
        }
    }
    res = dfb_seal(ctx, &B);
    return res;
fail:
    dfb_free(ctx, &B);
    return JS_EXCEPTION;
}

JSValue dyn_df_pivot(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* op = "PIVOT";
    DataFrame* df;
    DFBound bi, bc, bv;
    DfcSet iset = { 0 }, pset = { 0 };
    uint32_t* ifirst = NULL;
    uint32_t ni = 0, np = 0, n, i, pe;
    int ii, ic, iv, agg = DFR_SUM;
    const char* aggs = NULL;
    char** pnames = NULL;
    double *acc = NULL, *mn = NULL, *mx = NULL, *fst = NULL, *lst = NULL;
    uint32_t* cnt = NULL;
    DFBuilder B = { 0 };
    JSValue res;
    int do_acc, do_cnt, do_mn, do_mx, do_fl;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ii = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ii < 0)
        return JS_EXCEPTION;
    ic = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (ic < 0)
        return JS_EXCEPTION;
    iv = df_col_arg(ctx, df, argc > 2 ? argv[2] : JS_UNDEFINED);
    if (iv < 0)
        return JS_EXCEPTION;
    if (argc > 3 && !JS_IsUndefined(argv[3])) {
        aggs = JS_ToCString(ctx, argv[3]);
        if (!aggs)
            return JS_EXCEPTION;
        if (!strcmp(aggs, "sum"))
            agg = DFR_SUM;
        else if (!strcmp(aggs, "mean"))
            agg = DFR_MEAN;
        else if (!strcmp(aggs, "min"))
            agg = DFR_MIN;
        else if (!strcmp(aggs, "max"))
            agg = DFR_MAX;
        else if (!strcmp(aggs, "count"))
            agg = DFR_COUNT;
        else if (!strcmp(aggs, "first"))
            agg = 5;
        else if (!strcmp(aggs, "last"))
            agg = 6;
        else {
            JS_FreeCString(ctx, aggs);
            return JS_ThrowRangeError(ctx, "%s(..., agg): agg must be one of "
                                           "sum/mean/min/max/count/first/last",
                op);
        }
        JS_FreeCString(ctx, aggs);
    }
    if (df_bind_numeric(ctx, df, iv, &bv, op))
        return JS_EXCEPTION;
    if (dyn_df_bind(ctx, df, ii, &bi))
        return JS_EXCEPTION;
    if (dyn_df_bind(ctx, df, ic, &bc))
        return JS_EXCEPTION;

    n = bi.n < bc.n ? bi.n : bc.n;
    if (bv.n < n)
        n = bv.n;
    if (n > df->nrows)
        n = df->nrows;

    ifirst = df_out_alloc(ctx, (uint32_t)(n ? n : 1), sizeof(uint32_t));
    if (!ifirst)
        goto fail;
    for (i = 0; i < n; i++) {
        uint64_t ikey, pkey;
        uint32_t ie, pe_idx;
        int fr;
        if (bi.type == DF_STR)
            ikey = dfc_key((double)((const int32_t*)bi.p)[i]);
        else
            ikey = dfc_key(df_get(bi.p, bi.type, i));
        if (bc.type == DF_STR)
            pkey = dfc_key((double)((const int32_t*)bc.p)[i]);
        else
            pkey = dfc_key(df_get(bc.p, bc.type, i));
        if (dfc_set_put(ctx, &iset, ikey, 0, &ie, &fr))
            goto fail;
        if (fr) {
            ifirst[ie] = i;
            ni++;
        }
        if (dfc_set_put(ctx, &pset, pkey, 0, &pe_idx, &fr))
            goto fail;
        if (fr)
            np++;
    }
    if (np + 1 > DF_MAX_COLS) {
        JS_ThrowRangeError(ctx, "%s: result would be %u rows x %u columns; "
                                "the max is %u rows and %d columns (DF_MAX_COLS)",
            op, ni, np + 1, DFB_MAX_ROWS, DF_MAX_COLS);
        goto fail;
    }
    {
        uint64_t cells = (uint64_t)ni * (uint64_t)np;
        uint64_t per = 4;
        if (agg == DFR_SUM || agg == DFR_MEAN || agg == DFR_MIN || agg == DFR_MAX)
            per += 8;
        if (agg == 5 || agg == 6)
            per += 16;
        if (cells > DFP_CELL_BUDGET / per) {
            JS_ThrowRangeError(ctx, "%s: %u index values x %u pivot values "
                                    "needs %llu accumulator slots (about %llu MB); "
                                    "the max for this agg is %llu slots (%llu MB). "
                                    "Fewer distinct index values, or GROUP_BY first.",
                op, ni, np, (unsigned long long)cells,
                (unsigned long long)(cells * per / (1024 * 1024)),
                (unsigned long long)(DFP_CELL_BUDGET / per),
                (unsigned long long)(DFP_CELL_BUDGET / (1024 * 1024)));
            goto fail;
        }
    }
    pnames = calloc((size_t)(np ? np : 1), sizeof(char*));
    if (!pnames) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    for (pe = 0; pe < np; pe++) {
        uint64_t bits = pset.keys[pe];
        char tmp[32];
        if (bc.type == DF_STR) {
            uint32_t code = (uint32_t)dfc_key_value(bits);
            if (code >= df->cols[ic].dict_len) {
                JS_ThrowRangeError(ctx, "%s: pivot code %u out of range", op,
                    code);
                goto fail;
            }
            pnames[pe] = strdup(df->cols[ic].dict[code]);
        } else {
            df_fmt_double(tmp, sizeof(tmp), dfc_key_value(bits));
            pnames[pe] = strdup(tmp);
        }
        if (!pnames[pe]) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        if (strcmp(pnames[pe], df->cols[ii].name) == 0) {
            JS_ThrowRangeError(ctx, "%s: pivot value '%s' collides with the "
                                    "index column name",
                op, pnames[pe]);
            goto fail;
        }
    }

    do_acc = (agg == DFR_SUM || agg == DFR_MEAN);
    do_cnt = 1;
    do_mn = (agg == DFR_MIN);
    do_mx = (agg == DFR_MAX);
    do_fl = (agg == 5 || agg == 6);
    if (do_acc) {
        acc = calloc((size_t)ni * np, sizeof(double));
        if (!acc) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
    }
    cnt = calloc((size_t)ni * np, sizeof(uint32_t));
    if (!cnt) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    if (do_mn) {
        mn = malloc((size_t)ni * np * sizeof(double));
        if (!mn) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        for (i = 0; i < (uint64_t)ni * np; i++)
            mn[i] = DYN_INFINITY;
    }
    if (do_mx) {
        mx = malloc((size_t)ni * np * sizeof(double));
        if (!mx) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        for (i = 0; i < (uint64_t)ni * np; i++)
            mx[i] = -DYN_INFINITY;
    }
    if (do_fl) {
        fst = malloc((size_t)ni * np * sizeof(double));
        lst = malloc((size_t)ni * np * sizeof(double));
        if (!fst || !lst) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        for (i = 0; i < (uint64_t)ni * np; i++) {
            fst[i] = DYN_NAN;
            lst[i] = DYN_NAN;
        }
    }

    for (i = 0; i < n; i++) {
        uint64_t ikey, pkey;
        uint32_t ie, pe_idx;
        double v = df_get(bv.p, bv.type, i);
        if (bi.type == DF_STR)
            ikey = dfc_key((double)((const int32_t*)bi.p)[i]);
        else
            ikey = dfc_key(df_get(bi.p, bi.type, i));
        if (bc.type == DF_STR)
            pkey = dfc_key((double)((const int32_t*)bc.p)[i]);
        else
            pkey = dfc_key(df_get(bc.p, bc.type, i));
        if (dfc_set_find(&iset, ikey, 0, &ie) || dfc_set_find(&pset, pkey, 0, &pe_idx))
            continue;
        {
            uint64_t cell = (uint64_t)ie * np + pe_idx;
            if (do_acc)
                acc[cell] += v;
            if (do_cnt)
                cnt[cell]++;
            if (do_mn && v == v && v < mn[cell])
                mn[cell] = v;
            if (do_mx && v == v && v > mx[cell])
                mx[cell] = v;
            if (do_fl) {
                if (v == v && fst[cell] != fst[cell])
                    fst[cell] = v;
                if (v == v)
                    lst[cell] = v;
            }
        }
    }

    {
        uint32_t* irows = df_out_alloc(ctx, ni ? ni : 1, sizeof(uint32_t));
        uint32_t k, c;
        if (!irows)
            goto fail;
        for (k = 0; k < ni; k++)
            irows[k] = ifirst[k];
        if (dfb_init(ctx, &B, op, np + 1, ni)) {
            free(irows);
            goto fail;
        }
        if (dfb_add_gather(ctx, &B, df->cols[ii].name, df, ii, bi.type,
                irows, ni)) {
            free(irows);
            goto fail;
        }
        free(irows);
        for (pe = 0; pe < np; pe++) {
            double* col = df_out_alloc(ctx, ni ? ni : 1, sizeof(double));
            if (!col)
                goto fail;
            for (k = 0; k < ni; k++) {
                uint64_t cell = (uint64_t)k * np + pe;
                switch (agg) {
                case DFR_SUM:
                    col[k] = cnt[cell] ? acc[cell] : DYN_NAN;
                    break;
                case DFR_MEAN:
                    col[k] = cnt[cell] ? acc[cell] / cnt[cell] : DYN_NAN;
                    break;
                case DFR_MIN:
                    col[k] = (mn[cell] == DYN_INFINITY) ? DYN_NAN : mn[cell];
                    break;
                case DFR_MAX:
                    col[k] = (mx[cell] == -DYN_INFINITY) ? DYN_NAN : mx[cell];
                    break;
                case DFR_COUNT:
                    col[k] = (double)cnt[cell];
                    break;
                default:
                    col[k] = fst[cell];
                    break;
                }
                if (agg == 6)
                    col[k] = lst[cell];
            }
            if (dfb_add(ctx, &B, pnames[pe], DF_F64, col, ni)) {
                col = NULL;
                goto fail;
            }
        }
        (void)c;
    }
    res = dfb_seal(ctx, &B);
    dfc_set_free(&iset);
    dfc_set_free(&pset);
    free(ifirst);
    free(acc);
    free(cnt);
    free(mn);
    free(mx);
    free(fst);
    free(lst);
    for (pe = 0; pe < np; pe++)
        free(pnames[pe]);
    free(pnames);
    return res;
fail:
    dfc_set_free(&iset);
    dfc_set_free(&pset);
    free(ifirst);
    free(acc);
    free(cnt);
    free(mn);
    free(mx);
    free(fst);
    free(lst);
    if (pnames) {
        uint32_t pe_idx;
        for (pe_idx = 0; pe_idx < np; pe_idx++)
            free(pnames[pe_idx]);
        free(pnames);
    }
    dfb_free(ctx, &B);
    return JS_EXCEPTION;
}

JSValue dyn_df_melt(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* op = "MELT";
    DataFrame* df;
    DFBound* binds = NULL;
    uint32_t *idxs = NULL, *vvals = NULL, *rows = NULL;
    int32_t* varcodes = NULL;
    uint64_t nout;
    uint32_t i, k, c, nid = 0, nvv = 0, nrows;
    DFBuilder B = { 0 };
    JSValue res;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        if (!JS_IsArray(ctx, argv[0]))
            return JS_ThrowTypeError(ctx, "%s(idVars, valueVars): idVars must "
                                          "be an array of names",
                op);
    }
    if (argc < 2 || !JS_IsArray(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "%s(idVars, valueVars): valueVars must "
                                      "be an array of names",
            op);
    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lv) || JS_ToUint32(ctx, &nid, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    }
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[1], "length");
        if (JS_IsException(lv) || JS_ToUint32(ctx, &nvv, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    }
    if (nvv == 0)
        return JS_ThrowRangeError(ctx, "%s(idVars, valueVars): valueVars "
                                       "cannot be empty",
            op);
    idxs = df_out_alloc(ctx, nid ? nid : 1, sizeof(uint32_t));
    vvals = df_out_alloc(ctx, nvv ? nvv : 1, sizeof(uint32_t));
    if (!idxs || !vvals)
        goto fail;
    for (i = 0; i < nid; i++) {
        JSValue nv = JS_GetPropertyUint32(ctx, argv[0], i);
        int idx;
        if (JS_IsException(nv)) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        idx = df_col_arg(ctx, df, nv);
        JS_FreeValue(ctx, nv);
        if (idx < 0)
            goto fail;
        idxs[i] = (uint32_t)idx;
    }
    for (i = 0; i < nvv; i++) {
        JSValue nv = JS_GetPropertyUint32(ctx, argv[1], i);
        int idx;
        if (JS_IsException(nv)) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        idx = df_col_arg(ctx, df, nv);
        JS_FreeValue(ctx, nv);
        if (idx < 0)
            goto fail;
        if (df->cols[idx].type == DF_STR) {
            JS_ThrowTypeError(ctx, "%s: valueVar '%s' is a string column; "
                                   "melt needs numeric value columns",
                op, df->cols[idx].name);
            goto fail;
        }
        vvals[i] = (uint32_t)idx;
    }
    for (i = 0; i < nid; i++)
        for (k = 0; k < nvv; k++)
            if (idxs[i] == vvals[k]) {
                JS_ThrowRangeError(ctx, "%s: column '%s' is both an idVar and "
                                        "a valueVar",
                    op, df->cols[idxs[i]].name);
                goto fail;
            }

    nrows = df->nrows;
    nout = (uint64_t)nrows * nvv;
    if (nout > (uint64_t)DFB_MAX_ROWS) {
        JS_ThrowRangeError(ctx, "%s: result would have %llu rows (max %u)",
            op, (unsigned long long)nout, DFB_MAX_ROWS);
        goto fail;
    }
    rows = df_out_alloc(ctx, (uint32_t)(nout ? nout : 1), sizeof(uint32_t));
    varcodes = df_out_alloc(ctx, (uint32_t)(nout ? nout : 1), sizeof(int32_t));
    if (!rows || !varcodes)
        goto fail;
    for (i = 0; i < nout; i++) {
        rows[i] = (uint32_t)(i / nvv);
        varcodes[i] = (int32_t)(i % nvv);
    }

    if (dfb_init(ctx, &B, op, nid + 2, nout))
        goto fail;
    for (c = 0; c < nid; c++)
        if (dfb_add_gather(ctx, &B, df->cols[idxs[c]].name, df, (int)idxs[c],
                df->cols[idxs[c]].type, rows, (uint32_t)nout))
            goto fail;
    {
        DfStrTab tab = { 0 };
        int32_t* codes = df_out_alloc(ctx, (uint32_t)(nout ? nout : 1),
            sizeof(int32_t));
        int32_t* poscode = df_out_alloc(ctx, (uint32_t)(nvv ? nvv : 1),
            sizeof(int32_t));
        char** dict;
        uint32_t code;
        if (!codes || !poscode) {
            free(poscode);
            goto fail;
        }
        for (i = 0; i < nvv; i++) {
            if (df_strtab_put(ctx, &tab, df->cols[vvals[i]].name, &code)) {
                free(poscode);
                goto fail;
            }
            poscode[i] = (int32_t)code;
        }
        for (i = 0; i < nout; i++) {
            codes[i] = poscode[varcodes[i]];
        }
        free(poscode);
        dict = tab.dict;
        if (dfb_add_str(ctx, &B, "variable", codes, dict, tab.dict_len,
                (uint32_t)nout)) {
            tab.dict = NULL;
            tab.dict_len = 0;
            df_strtab_free(&tab);
            goto fail;
        }
        tab.dict = NULL;
        tab.dict_len = 0;
        df_strtab_free(&tab);
    }
    {
        double* vals = df_out_alloc(ctx, (uint32_t)(nout ? nout : 1),
            sizeof(double));
        if (!vals)
            goto fail;
        binds = calloc(nvv, sizeof(DFBound));
        if (!binds) {
            free(vals);
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        for (k = 0; k < nvv; k++)
            if (dyn_df_bind(ctx, df, (int)vvals[k], &binds[k])) {
                free(vals);
                goto fail;
            }
        for (i = 0; i < nout; i++)
            vals[i] = df_get(binds[varcodes[i]].p, binds[varcodes[i]].type,
                rows[i]);
        if (dfb_add(ctx, &B, "value", DF_F64, vals, (uint32_t)nout)) {
            vals = NULL;
            goto fail;
        }
    }
    free(binds);
    free(idxs);
    free(vvals);
    free(rows);
    free(varcodes);
    res = dfb_seal(ctx, &B);
    return res;
fail:
    free(binds);
    free(idxs);
    free(vvals);
    free(rows);
    free(varcodes);
    dfb_free(ctx, &B);
    return JS_EXCEPTION;
}

#endif
