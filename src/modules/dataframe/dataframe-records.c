#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

JSValue dyn_df_get_columns(JSContext* ctx, JSValueConst this_val)
{
    DataFrame* df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    JSValue a;
    uint32_t i;
    if (!df)
        return JS_EXCEPTION;
    a = JS_NewArray(ctx);
    if (JS_IsException(a))
        return a;
    for (i = 0; i < df->ncols; i++) {
        JSValue s = JS_NewString(ctx, df->cols[i].name);
        if (JS_IsException(s) || JS_DefinePropertyValueUint32(ctx, a, i, s, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, a);
            return JS_EXCEPTION;
        }
    }
    return a;
}

JSValue dyn_df_to_columns(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    JSValue res;
    uint32_t i;

    (void)argc;
    (void)argv;
    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        return res;
    for (i = 0; i < df->ncols; i++) {
        JSValue v;
        if (df->cols[i].type == DF_STR) {
            uint32_t n, k;
            DFType t;
            int32_t* codes = df_col_copy(ctx, df, (int)i, &n, &t);
            if (!codes) {
                JS_FreeValue(ctx, res);
                return JS_EXCEPTION;
            }
            v = JS_NewArray(ctx);
            if (JS_IsException(v)) {
                free(codes);
                JS_FreeValue(ctx, res);
                return JS_EXCEPTION;
            }
            for (k = 0; k < n; k++) {
                int32_t c = codes[k];
                if (c < 0 || (uint32_t)c >= df->cols[i].dict_len) {
                    JS_FreeValue(ctx, v);
                    free(codes);
                    JS_FreeValue(ctx, res);
                    return JS_ThrowRangeError(ctx, "TO_COLUMNS: column '%s' "
                                                   "code %d out of range",
                        df->cols[i].name, c);
                }
                if (JS_DefinePropertyValueUint32(ctx, v, k,
                        JS_NewString(ctx, df->cols[i].dict[c]),
                        JS_PROP_C_W_E)
                    < 0) {
                    JS_FreeValue(ctx, v);
                    free(codes);
                    JS_FreeValue(ctx, res);
                    return JS_EXCEPTION;
                }
            }
            free(codes);
        } else {
            DFBound b;
            void* out;
            if (dyn_df_bind(ctx, df, (int)i, &b))
                goto fail;
            out = df_out_alloc(ctx, df->nrows ? df->nrows : 1, df_elt_size(b.type));
            if (!out)
                goto fail;
            memcpy(out, b.p, (size_t)df->nrows * df_elt_size(b.type));
            v = df_to_typed_array(ctx, out, (size_t)df->nrows * df_elt_size(b.type),
                df_type_ta(b.type));
            if (JS_IsException(v))
                goto fail;
        }
        if (JS_DefinePropertyValueStr(ctx, res, df->cols[i].name, v,
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, v);
            goto fail;
        }
    }
    return res;
fail:
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}

JSValue dyn_df_to_records(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    JSValue res = JS_EXCEPTION, *rows = NULL, *vals = NULL, **strs = NULL;
    DFBound* bs = NULL;
    JSAtom *atoms = NULL, *ra = NULL;
    JSShapeCache sc = { 0 };
    uint32_t n, c, r, k, natoms = 0, nrows_made = 0, ncols;

    (void)argc;
    (void)argv;
    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ncols = df->ncols;
    n = df->nrows;
    bs = (DFBound*)malloc((ncols ? ncols : 1) * sizeof(*bs));
    atoms = (JSAtom*)malloc((ncols ? ncols : 1) * sizeof(*atoms));
    ra = (JSAtom*)malloc((ncols ? ncols : 1) * sizeof(*ra));
    vals = (JSValue*)malloc((ncols ? ncols : 1) * sizeof(*vals));
    strs = (JSValue**)calloc(ncols ? ncols : 1, sizeof(*strs));
    rows = (JSValue*)malloc((n ? n : 1) * sizeof(*rows));
    if (!bs || !atoms || !ra || !vals || !strs || !rows) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    for (c = 0; c < ncols; c++) {
        if (dyn_df_bind(ctx, df, (int)c, &bs[c]))
            goto done;
        atoms[c] = JS_NewAtom(ctx, df->cols[c].name);
        if (atoms[c] == JS_ATOM_NULL)
            goto done;
        natoms = c + 1;
        if (df->cols[c].type == DF_STR) {
            uint32_t d, dl = df->cols[c].dict_len;
            strs[c] = (JSValue*)malloc((dl ? dl : 1) * sizeof(JSValue));
            if (!strs[c]) {
                JS_ThrowOutOfMemory(ctx);
                goto done;
            }
            for (d = 0; d < dl; d++)
                strs[c][d] = JS_UNDEFINED;
        }
    }
    for (r = 0; r < n; r++) {
        for (c = 0; c < ncols; c++) {
            JSValue v;
            if (df->cols[c].type == DF_STR) {
                int32_t code = ((const int32_t*)bs[c].p)[r];
                if (code < 0 || (uint32_t)code >= df->cols[c].dict_len) {
                    for (k = 0; k < c; k++)
                        JS_FreeValue(ctx, vals[k]);
                    JS_ThrowRangeError(ctx, "TO_RECORDS: column '%s' "
                                            "code %d out of range",
                        df->cols[c].name, code);
                    goto done;
                }
                if (JS_IsUndefined(strs[c][code])) {
                    strs[c][code] = JS_NewString(ctx, df->cols[c].dict[code]);
                    if (JS_IsException(strs[c][code])) {
                        strs[c][code] = JS_UNDEFINED;
                        for (k = 0; k < c; k++)
                            JS_FreeValue(ctx, vals[k]);
                        goto done;
                    }
                }
                v = JS_DupValue(ctx, strs[c][code]);
            } else {
                v = JS_NewFloat64(ctx, df_get(bs[c].p, bs[c].type, r));
            }
            vals[c] = v;
            ra[c] = JS_DupAtom(ctx, atoms[c]);
        }
        rows[r] = JS_NewObjectFromProps(ctx, &sc, (int)ncols, ra, vals);
        if (JS_IsException(rows[r]))
            goto done;
        nrows_made = r + 1;
    }
    nrows_made = 0;
    res = JS_NewArrayFrom(ctx, (int)n, rows);
done:
    for (r = 0; r < nrows_made; r++)
        JS_FreeValue(ctx, rows[r]);
    if (strs) {
        for (c = 0; c < ncols; c++)
            if (strs[c]) {
                uint32_t d;
                for (d = 0; d < df->cols[c].dict_len; d++)
                    JS_FreeValue(ctx, strs[c][d]);
                free(strs[c]);
            }
    }
    JS_FreeShapeCache(JS_GetRuntime(ctx), &sc);
    for (c = 0; c < natoms; c++)
        JS_FreeAtom(ctx, atoms[c]);
    free(strs);
    free(rows);
    free(vals);
    free(ra);
    free(atoms);
    free(bs);
    return res;
}

JSValue dyn_df_to_json(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue recs, s;

    (void)argc;
    (void)argv;
    recs = dyn_df_to_records(ctx, this_val, 0, argv);
    if (JS_IsException(recs))
        return JS_EXCEPTION;
    s = JS_JSONStringify(ctx, recs, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue(ctx, recs);
    return s;
}

JSValue dyn_df_from_records(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue rows, lenv, res;
    uint32_t nrows, i, c, ncols = 0;
    uint32_t nkeys;
    JSPropertyEnum* tab = NULL;
    const char** names = NULL;
    JSAtom* col_atoms = NULL;
    uint32_t* aset = NULL;
    JSPropCache* col_pcs = NULL;
    int* is_num = NULL;
    void** colbuf = NULL;
    DfStrTab* strtabs = NULL;
    JSValue prev = JS_UNDEFINED;
    DFBuilder B = { 0 };

    (void)this_val;
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "FROM_RECORDS(rows): expected an array of "
                                      "row objects");
    rows = argv[0];
    lenv = JS_GetPropertyStr(ctx, rows, "length");
    if (JS_IsException(lenv))
        return JS_EXCEPTION;
    if (JS_ToUint32(ctx, &nrows, lenv)) {
        JS_FreeValue(ctx, lenv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lenv);
    if (nrows == 0)
        return JS_ThrowRangeError(ctx, "FROM_RECORDS(rows): cannot infer a "
                                       "schema from zero rows");

    names = malloc((size_t)DF_MAX_COLS * sizeof(char*));
    col_atoms = malloc((size_t)DF_MAX_COLS * sizeof(JSAtom));
    col_pcs = calloc((size_t)DF_MAX_COLS, sizeof(JSPropCache));
    aset = calloc((size_t)DF_MAX_COLS * 2, sizeof(uint32_t));
    if (!names || !col_atoms || !col_pcs || !aset) {
        free(names);
        free(col_atoms);
        free(col_pcs);
        free(aset);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < nrows; i++) {
        JSValue row = JS_GetPropertyUint32(ctx, rows, i);
        uint32_t k;
        if (JS_IsException(row)) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        if (JS_SameShape(row, prev)) {
            JS_FreeValue(ctx, prev);
            prev = row;
            continue;
        }
        if (!JS_IsObject(row) || JS_IsArray(ctx, row)) {
            JS_FreeValue(ctx, row);
            JS_ThrowTypeError(ctx, "FROM_RECORDS(rows): row %u is not a plain "
                                   "object",
                i);
            goto fail;
        }
        if (JS_GetOwnPropertyNames(ctx, &tab, &nkeys, row,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
            < 0) {
            JS_FreeValue(ctx, row);
            goto fail;
        }
        for (k = 0; k < nkeys; k++) {
            JSValue nv;
            size_t nlen = 0;
            const char* nm;
            uint32_t j = (tab[k].atom * 2654435761u) & (DF_MAX_COLS * 2 - 1);
            int seen = k < ncols && col_atoms[k] == tab[k].atom;
            for (; !seen && aset[j]; j = (j + 1) & (DF_MAX_COLS * 2 - 1))
                seen = aset[j] == tab[k].atom;
            if (seen)
                continue;
            nv = JS_AtomToValue(ctx, tab[k].atom);
            nm = JS_IsException(nv) ? NULL : JS_ToCStringLen(ctx, &nlen, nv);
            JS_FreeValue(ctx, nv);
            if (!nm) {
                JS_FreeValue(ctx, row);
                goto fail;
            }
            if (strlen(nm) != nlen) {
                JS_FreeCString(ctx, nm);
                JS_FreeValue(ctx, row);
                JS_ThrowRangeError(ctx, "FROM_RECORDS: column name contains a "
                                        "NUL character");
                goto fail;
            }
            {
                if (ncols >= DF_MAX_COLS) {
                    JS_FreeCString(ctx, nm);
                    JS_FreeValue(ctx, row);
                    JS_ThrowRangeError(ctx, "FROM_RECORDS: too many columns "
                                            "(max %d)",
                        DF_MAX_COLS);
                    goto fail;
                }
                names[ncols] = strdup(nm);
                if (!names[ncols]) {
                    JS_FreeCString(ctx, nm);
                    JS_FreeValue(ctx, row);
                    JS_ThrowOutOfMemory(ctx);
                    goto fail;
                }
                col_atoms[ncols] = JS_DupAtom(ctx, tab[k].atom);
                aset[j] = tab[k].atom;
                ncols++;
            }
            JS_FreeCString(ctx, nm);
        }
        for (k = 0; k < nkeys; k++)
            JS_FreeAtom(ctx, tab[k].atom);
        js_free(ctx, tab);
        tab = NULL;
        JS_FreeValue(ctx, prev);
        prev = row;
    }
    JS_FreeValue(ctx, prev);
    prev = JS_UNDEFINED;

    is_num = malloc((size_t)(ncols ? ncols : 1) * sizeof(int));
    colbuf = calloc((size_t)(ncols ? ncols : 1), sizeof(void*));
    strtabs = calloc((size_t)(ncols ? ncols : 1), sizeof(DfStrTab));
    if (!is_num || !colbuf || !strtabs) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    for (c = 0; c < ncols; c++)
        is_num[c] = 1;
    for (i = 0; i < nrows; i++) {
        JSValue row = JS_GetPropertyUint32(ctx, rows, i);
        if (JS_IsException(row))
            goto fail;
        for (c = 0; c < ncols; c++) {
            JSValue v;
            if (!is_num[c])
                continue;
            v = JS_GetPropertyCached(ctx, row, col_atoms[c], &col_pcs[c]);
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, row);
                goto fail;
            }
            if (!JS_IsUndefined(v) && !JS_IsNumber(v))
                is_num[c] = 0;
            JS_FreeValue(ctx, v);
        }
        JS_FreeValue(ctx, row);
    }

    if (dfb_init(ctx, &B, "FROM_RECORDS", ncols, nrows))
        goto fail;
    for (c = 0; c < ncols; c++) {
        colbuf[c] = df_out_alloc(ctx, nrows ? nrows : 1, is_num[c] ? sizeof(double) : sizeof(int32_t));
        if (!colbuf[c])
            goto fail;
    }
    for (i = 0; i < nrows; i++) {
        JSValue row = JS_GetPropertyUint32(ctx, rows, i);
        if (JS_IsException(row))
            goto fail;
        for (c = 0; c < ncols; c++) {
            JSValue v = JS_GetPropertyCached(ctx, row, col_atoms[c], &col_pcs[c]);
            uint32_t code;
            const char* s;
            int owned, put;
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, row);
                goto fail;
            }
            if (is_num[c]) {
                double* vals = (double*)colbuf[c];
                if (JS_IsUndefined(v)) {
                    vals[i] = DYN_NAN;
                } else if (JS_ToFloat64(ctx, &vals[i], v)) {
                    JS_FreeValue(ctx, v);
                    JS_FreeValue(ctx, row);
                    goto fail;
                }
                JS_FreeValue(ctx, v);
                continue;
            }
            owned = !JS_IsUndefined(v);
            if (!owned) {
                s = "";
            } else {
                s = JS_ToCString(ctx, v);
                JS_FreeValue(ctx, v);
                if (!s) {
                    JS_FreeValue(ctx, row);
                    goto fail;
                }
            }
            put = df_strtab_put(ctx, &strtabs[c], s, &code);
            if (owned)
                JS_FreeCString(ctx, s);
            if (put) {
                JS_FreeValue(ctx, row);
                goto fail;
            }
            ((int32_t*)colbuf[c])[i] = (int32_t)code;
        }
        JS_FreeValue(ctx, row);
    }
    for (c = 0; c < ncols; c++) {
        void* buf = colbuf[c];
        colbuf[c] = NULL;
        if (is_num[c]) {
            if (dfb_add(ctx, &B, names[c], DF_F64, buf, nrows))
                goto fail;
        } else {
            int bad = dfb_add_str(ctx, &B, names[c], (int32_t*)buf, strtabs[c].dict,
                strtabs[c].dict_len, nrows);
            strtabs[c].dict = NULL;
            strtabs[c].dict_len = 0;
            df_strtab_free(&strtabs[c]);
            if (bad)
                goto fail;
        }
    }
    res = dfb_seal(ctx, &B);
    for (i = 0; i < ncols; i++) {
        free(DYN_UNCONST(names[i]));
        JS_FreeAtom(ctx, col_atoms[i]);
        JS_FreePropCache(JS_GetRuntime(ctx), &col_pcs[i]);
    }
    free(col_pcs);
    free(col_atoms);
    free(aset);
    free(names);
    free(is_num);
    free(colbuf);
    free(strtabs);
    return res;
fail:
    JS_FreeValue(ctx, prev);
    if (tab) {
        uint32_t k;
        for (k = 0; k < nkeys; k++)
            JS_FreeAtom(ctx, tab[k].atom);
        js_free(ctx, tab);
    }
    if (colbuf)
        for (c = 0; c < ncols; c++)
            free(colbuf[c]);
    if (strtabs)
        for (c = 0; c < ncols; c++)
            df_strtab_free(&strtabs[c]);
    free(colbuf);
    free(strtabs);
    dfb_free(ctx, &B);
    for (i = 0; i < ncols; i++) {
        free(DYN_UNCONST(names[i]));
        JS_FreeAtom(ctx, col_atoms[i]);
        JS_FreePropCache(JS_GetRuntime(ctx), &col_pcs[i]);
    }
    free(col_pcs);
    free(col_atoms);
    free(aset);
    free(names);
    free(is_num);
    return JS_EXCEPTION;
}

JSValue dyn_df_json_agg(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* op = (magic == DFJA_AGG || magic == DFJA_AGG_STRICT)
        ? "JSON_AGG"
        : "JSON_OBJECT_AGG";
    DataFrame* df;
    DFBound bk, bv;
    const uint8_t* mask;
    double *keys = NULL, *vals = NULL;
    uint32_t i, n, avail;
    int ki, vi, ok, strict;
    JSValue res, j;

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
    strict = (magic == DFJA_AGG_STRICT || magic == DFJA_OBJ_STRICT);

    if (dyn_df_bind(ctx, df, ki, &bk))
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, vi, &bv, op))
        return JS_EXCEPTION;
    avail = bk.n < bv.n ? bk.n : bv.n;
    n = avail < df->nrows ? avail : df->nrows;

    keys = df_out_alloc(ctx, n ? n : 1, sizeof(double));
    vals = df_out_alloc(ctx, n ? n : 1, sizeof(double));
    if (!keys || !vals) {
        free(keys);
        free(vals);
        return JS_EXCEPTION;
    }
    for (i = 0; i < n; i++) {
        if (mask && !mask[i]) {
            keys[i] = 0.0;
            vals[i] = 0.0;
            continue;
        }
        keys[i] = (bk.type == DF_STR) ? (double)((const int32_t*)bk.p)[i]
                                      : df_get(bk.p, bk.type, i);
        vals[i] = df_get(bv.p, bv.type, i);
    }

    res = dfja_build(ctx, df, ki, keys, vals, mask, n,
        magic == DFJA_OBJ || magic == DFJA_OBJ_STRICT, strict);
    free(keys);
    free(vals);
    if (JS_IsException(res))
        return JS_EXCEPTION;
    j = JS_JSONStringify(ctx, res, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue(ctx, res);
    return j;
}

#endif
