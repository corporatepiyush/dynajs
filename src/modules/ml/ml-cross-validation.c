#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

static const char* const ml_sel_fold_keys[] = { "seed", "shuffle", "k",
    "folds" };

static const char* const ml_crossval_keys[] = { "scoring", "seed", "shuffle",
    "k", "folds", "nJobs",
    "onProgress" };

JSValue dyn_ml_kfold_impl(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t seed = 12345;
    int shuffle = 0;
    int32_t k = 5;
    double nd;
    uint32_t *idx = NULL, *tr = NULL;
    size_t n, i, f, start = 0;
    JSValue out;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ml.kFold(n[, options])");
    if (JS_IsObject(argv[0])) {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (JS_ToFloat64(ctx, &nd, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    } else if (JS_ToFloat64(ctx, &nd, argv[0])) {
        return JS_EXCEPTION;
    }
    if (dyn_ml_sel_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
            &seed, &shuffle, NULL, &k)
        < 0)
        return JS_EXCEPTION;
    if (!(nd >= 2.0) || nd != floor(nd) || nd > 1e8)
        return JS_ThrowRangeError(ctx, "ml.kFold: needs at least 2 samples");
    n = (size_t)nd;
    if (k < 2 || (size_t)k > n)
        return JS_ThrowRangeError(ctx, "ml.kFold: k must be in [2, n]");
    if ((size_t)k > 20000000 / n)
        return JS_ThrowRangeError(ctx,
            "ml.kFold: k * n exceeds the output index budget (20000000)");

    idx = (uint32_t*)malloc(n * sizeof(uint32_t));
    tr = (uint32_t*)malloc(n * sizeof(uint32_t));
    if (!idx || !tr) {
        free(idx);
        free(tr);
        return JS_ThrowOutOfMemory(ctx);
    }
    dyn_fiber_offload();
    for (i = 0; i < n; i++)
        idx[i] = (uint32_t)i;
    if (shuffle)
        dyn_ml_shuffle_idx(idx, n, &seed);

    out = JS_NewArray(ctx);
    if (JS_IsException(out)) {
        free(idx);
        free(tr);
        return JS_EXCEPTION;
    }
    for (f = 0; f < (size_t)k; f++) {
        size_t sz = n / (size_t)k + (f < n % (size_t)k ? 1 : 0), ntr = 0, j;
        JSValue e;
        for (j = 0; j < n; j++)
            if (j < start || j >= start + sz)
                tr[ntr++] = idx[j];
        e = dyn_ml_split_obj(ctx, tr, ntr, idx + start, sz);
        if (JS_IsException(e) || JS_DefinePropertyValueUint32(ctx, out, (uint32_t)f, e, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, out);
            free(idx);
            free(tr);
            return JS_EXCEPTION;
        }
        start += sz;
    }
    free(idx);
    free(tr);
    return out;
}

JSValue dyn_ml_kfold(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    if (dyn_opts_strict(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
            ml_sel_fold_keys, 4))
        return JS_EXCEPTION;
    return dyn_ml_kfold_impl(ctx, this_val, argc, argv);
}

JSValue dyn_ml_stratified_kfold(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t seed = 12345;
    int shuffle = 0;
    int32_t k = 5;
    double* y = NULL;
    uint32_t *idx = NULL, *fold = NULL, *tr = NULL, *te = NULL;
    size_t n = 0, i, f;
    JSValue out, lv;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ml.stratifiedKFold(y[, options])");
    if (dyn_opts_strict(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
            ml_sel_fold_keys, 4))
        return JS_EXCEPTION;
    if (dyn_ml_sel_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
            &seed, &shuffle, NULL, &k)
        < 0)
        return JS_EXCEPTION;
    if (!JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "ml.stratifiedKFold: y must be an array");
    lv = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_IsException(lv))
        return JS_EXCEPTION;
    {
        double nd;
        if (JS_ToFloat64(ctx, &nd, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
        if (!(nd >= 2.0) || nd > 1e8)
            return JS_ThrowRangeError(ctx, "ml.stratifiedKFold: needs at least 2 samples");
        n = (size_t)nd;
    }
    if (k < 2 || (size_t)k > n)
        return JS_ThrowRangeError(ctx, "ml.stratifiedKFold: k must be in [2, n]");
    if ((size_t)k > 20000000 / n)
        return JS_ThrowRangeError(ctx,
            "ml.stratifiedKFold: k * n exceeds the output index budget (20000000)");
    y = (double*)malloc(n * sizeof(double));
    if (!y)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        if (JS_IsException(e) || JS_ToFloat64(ctx, &y[i], e)) {
            JS_FreeValue(ctx, e);
            free(y);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, e);
        if (!isfinite(y[i])) {
            free(y);
            return JS_ThrowRangeError(ctx, "ml.stratifiedKFold: labels must be finite");
        }
    }

    idx = (uint32_t*)malloc(n * sizeof(uint32_t));
    fold = (uint32_t*)malloc(n * sizeof(uint32_t));
    tr = (uint32_t*)malloc(n * sizeof(uint32_t));
    te = (uint32_t*)malloc(n * sizeof(uint32_t));
    if (!idx || !fold || !tr || !te) {
        free(y);
        free(idx);
        free(fold);
        free(tr);
        free(te);
        return JS_ThrowOutOfMemory(ctx);
    }
    dyn_fiber_offload();
    for (i = 0; i < n; i++) {
        idx[i] = (uint32_t)i;
        fold[i] = 0;
    }
    if (shuffle)
        dyn_ml_shuffle_idx(idx, n, &seed);

    {
        dyn_tree_kv_t* kv = (dyn_tree_kv_t*)malloc(n * sizeof(*kv));
        size_t run, pos, cnt;
        if (!kv) {
            free(y);
            free(idx);
            free(fold);
            free(tr);
            free(te);
            return JS_ThrowOutOfMemory(ctx);
        }
        for (i = 0; i < n; i++) {
            kv[i].key = y[idx[i]];
            kv[i].row = (uint32_t)i;
        }
        dyn_ml_isort(kv, n, sizeof(*kv), dyn_ml_kv_cmp);
        run = 0;
        while (run < n) {
            double k0 = kv[run].key;
            uint32_t foldno = 0;
            cnt = 0;
            for (pos = run; pos < n && kv[pos].key == k0; pos++, cnt++) {
                fold[idx[kv[pos].row]] = foldno + 1u;
                if (++foldno == (uint32_t)k)
                    foldno = 0;
            }
            if (cnt < (size_t)k) {
                JS_ThrowRangeError(ctx,
                    "ml.stratifiedKFold: a class has %u member(s) but k = %d; "
                    "k must not exceed the smallest class count",
                    (unsigned)cnt, (int)k);
                free(kv);
                goto fail;
            }
            run = pos;
        }
        free(kv);
    }

    out = JS_NewArray(ctx);
    if (JS_IsException(out))
        goto fail;
    for (f = 0; f < (size_t)k; f++) {
        size_t ntr = 0, nte = 0;
        JSValue e;
        for (i = 0; i < n; i++) {
            if (fold[i] == (uint32_t)f + 1u)
                te[nte++] = (uint32_t)i;
            else
                tr[ntr++] = (uint32_t)i;
        }
        e = dyn_ml_split_obj(ctx, tr, ntr, te, nte);
        if (JS_IsException(e) || JS_DefinePropertyValueUint32(ctx, out, (uint32_t)f, e, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, out);
            goto fail;
        }
    }
    free(y);
    free(idx);
    free(fold);
    free(tr);
    free(te);
    return out;
fail:
    free(y);
    free(idx);
    free(fold);
    free(tr);
    free(te);
    return JS_EXCEPTION;
}

JSValue dyn_ml_cross_val_score(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_matrix_t mx = { 0 };
    double *y = NULL, *scores = NULL;
    JSValue folds = JS_UNDEFINED, scorer = JS_UNDEFINED, out = JS_EXCEPTION;
    JSValue onprogress = JS_UNDEFINED;
    JSValueConst opts = argc > 3 ? argv[3] : JS_UNDEFINED;
    size_t nfold, f, njobs = 1;
    (void)this_val;

    if (argc < 3 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx,
            "ml.crossValScore(estimatorFactory, X, y[, options])");
    if (dyn_opts_strict(ctx, argc > 3 ? argv[3] : JS_UNDEFINED,
            ml_crossval_keys,
            (int)(sizeof(ml_crossval_keys) / sizeof(ml_crossval_keys[0]))))
        return JS_EXCEPTION;
    if (dyn_ml_parse_jobs(ctx, opts, &njobs, &onprogress))
        return JS_EXCEPTION;
    if (dyn_ml_search_ingest(ctx, argv[1], argv[2], &mx, &y))
        return JS_EXCEPTION;
    if (JS_IsObject(opts)) {
        scorer = JS_GetPropertyStr(ctx, opts, "scoring");
        if (JS_IsException(scorer))
            goto done;
    }
    if (dyn_ml_folds(ctx, opts, mx.rows, &folds))
        goto done;
    if (dyn_ml_len(ctx, folds, &nfold))
        goto done;
    scores = (double*)malloc((nfold ? nfold : 1) * sizeof(double));
    if (!scores) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    for (f = 0; f < nfold; f++) {
        JSValue fold = JS_GetPropertyUint32(ctx, folds, (uint32_t)f);
        JSValue trv, tev;
        uint32_t *tr = NULL, *te = NULL;
        size_t ntr = 0, nte = 0;
        int bad;
        if (JS_IsException(fold))
            goto done;
        trv = JS_GetPropertyStr(ctx, fold, "train");
        tev = JS_GetPropertyStr(ctx, fold, "test");
        JS_FreeValue(ctx, fold);
        bad = JS_IsException(trv) || JS_IsException(tev) || dyn_ml_read_idx(ctx, trv, &tr, &ntr, mx.rows) || dyn_ml_read_idx(ctx, tev, &te, &nte, mx.rows);
        JS_FreeValue(ctx, trv);
        JS_FreeValue(ctx, tev);
        if (bad || dyn_ml_score_fold(ctx, argv[0], scorer, &mx, y, tr, ntr, te, nte, &scores[f])) {
            free(tr);
            free(te);
            goto done;
        }
        free(tr);
        free(te);
        if (dyn_ml_progress(ctx, onprogress, f + 1, nfold))
            goto done;
    }
    out = dyn_ml_doubles_to_js(ctx, scores, nfold);
done:
    dyn_matrix_free(&mx);
    free(y);
    free(scores);
    JS_FreeValue(ctx, folds);
    JS_FreeValue(ctx, scorer);
    JS_FreeValue(ctx, onprogress);
    return out;
}

#endif
