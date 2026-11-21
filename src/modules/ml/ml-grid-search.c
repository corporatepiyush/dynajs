#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

#define js_free(...) (dyn_fiber_onjs(), js_free(__VA_ARGS__))
#define JS_FreeAtom(...) (dyn_fiber_onjs(), JS_FreeAtom(__VA_ARGS__))
#define JS_GetOwnPropertyNames(...) (dyn_fiber_onjs(), JS_GetOwnPropertyNames(__VA_ARGS__))
#define JS_AtomToString(...) (dyn_fiber_onjs(), JS_AtomToString(__VA_ARGS__))

static const char* const ml_search_keys[] = { "scoring", "seed", "nIter",
    "shuffle", "k", "folds",
    "nJobs", "onProgress" };

int dyn_ml_search_ingest(JSContext* ctx, JSValueConst xv, JSValueConst yv,
    dyn_matrix_t* mx, double** py)
{
    if (JS_IsArray(ctx, xv))
        return dyn_ml_ingest_Xy_no_w(ctx, xv, yv, JS_UNDEFINED, JS_UNDEFINED,
            mx, py);
    {
        size_t rows, tot, cols;
        double* src;
        JSValue rv, cv;
        int ret;
        if (JS_IsArray(ctx, yv)) {
            if (dyn_ml_len(ctx, yv, &rows))
                return -1;
        } else {
            if (dyn_ml_get_f64(ctx, yv, &src, &rows))
                return -1;
        }
        if (rows == 0) {
            JS_ThrowTypeError(ctx, "dyna:ml y must have at least one entry");
            return -1;
        }
        if (dyn_ml_get_f64(ctx, xv, &src, &tot))
            return -1;
        if (tot % rows != 0 || (cols = tot / rows) == 0) {
            JS_ThrowTypeError(ctx,
                "dyna:ml flat Float64Array X length must equal rows*cols");
            return -1;
        }
        rv = JS_NewInt64(ctx, (int64_t)rows);
        cv = JS_NewInt64(ctx, (int64_t)cols);
        ret = dyn_ml_ingest_Xy_no_w(ctx, xv, yv, rv, cv, mx, py);
        JS_FreeValue(ctx, rv);
        JS_FreeValue(ctx, cv);
        if (ret)
            return -1;
        if (!mx->owned) {
            double* copy = (double*)malloc(mx->rows * mx->cols * sizeof(double));
            if (!copy) {
                dyn_matrix_free(mx);
                free(*py);
                *py = NULL;
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            memcpy(copy, mx->data, mx->rows * mx->cols * sizeof(double));
            mx->data = copy;
            mx->owned = 1;
        }
    }
    return 0;
}

JSValue dyn_ml_grid_search(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int random)
{
    dyn_matrix_t mx = { 0 };
    double *y = NULL, *scores = NULL;
    JSValue folds = JS_UNDEFINED, scorer = JS_UNDEFINED, out = JS_EXCEPTION;
    JSValue results = JS_UNDEFINED, best = JS_UNDEFINED, keys = JS_UNDEFINED;
    JSValueConst opts = argc > 4 ? argv[4] : JS_UNDEFINED;
    size_t nfold = 0, nkey = 0, combo, k, f;
    size_t *lens = NULL, *pos = NULL;
    size_t njobs = 1, work = 0;
    JSValue onprogress = JS_UNDEFINED;
    uint64_t seed = 12345;
    int64_t n_iter = 10;
    double bestScore = -1e308;
    uint64_t total = 1;
    (void)this_val;

    if (argc < 4 || !JS_IsFunction(ctx, argv[0]) || !JS_IsObject(argv[3]))
        return JS_ThrowTypeError(ctx, random ? "ml.randomSearch(estimatorFactory, X, y, grid[, options])" : "ml.gridSearch(estimatorFactory, X, y, grid[, options])");
    if (dyn_opts_strict(ctx, argc > 4 ? argv[4] : JS_UNDEFINED,
            ml_search_keys,
            (int)(sizeof(ml_search_keys) / sizeof(ml_search_keys[0]))))
        return JS_EXCEPTION;
    if (dyn_ml_parse_jobs(ctx, opts, &njobs, &onprogress))
        return JS_EXCEPTION;
    if (dyn_ml_search_ingest(ctx, argv[1], argv[2], &mx, &y))
        return JS_EXCEPTION;

    if (JS_IsObject(opts)) {
        size_t sz = (size_t)seed;
        scorer = JS_GetPropertyStr(ctx, opts, "scoring");
        if (JS_IsException(scorer))
            goto done;
        if (dyn_opt_size(ctx, opts, "seed", &sz, 0))
            goto done;
        seed = (uint64_t)sz;
        {
            JSValue v = JS_GetPropertyStr(ctx, opts, "nIter");
            if (JS_IsException(v))
                goto done;
            if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &n_iter, v)) {
                JS_FreeValue(ctx, v);
                goto done;
            }
            JS_FreeValue(ctx, v);
        }
    }
    if (n_iter < 1) {
        JS_ThrowRangeError(ctx, "dyna:ml nIter must be at least 1");
        goto done;
    }
    if (dyn_ml_folds(ctx, opts, mx.rows, &folds) || dyn_ml_len(ctx, folds, &nfold))
        goto done;

    {
        JSPropertyEnum* tab;
        uint32_t cnt, i;
        if (JS_GetOwnPropertyNames(ctx, &tab, &cnt, argv[3],
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
            < 0)
            goto done;
        keys = JS_NewArray(ctx);
        if (JS_IsException(keys)) {
            for (i = 0; i < cnt; i++)
                JS_FreeAtom(ctx, tab[i].atom);
            js_free(ctx, tab);
            goto done;
        }
        for (i = 0; i < cnt; i++) {
            JSValue kn = JS_AtomToString(ctx, tab[i].atom);
            JS_FreeAtom(ctx, tab[i].atom);
            if (JS_IsException(kn) || JS_DefinePropertyValueUint32(ctx, keys, i, kn, JS_PROP_C_W_E) < 0) {
                for (++i; i < cnt; i++)
                    JS_FreeAtom(ctx, tab[i].atom);
                js_free(ctx, tab);
                goto done;
            }
        }
        js_free(ctx, tab);
        nkey = cnt;
    }
    if (nkey == 0) {
        JS_ThrowTypeError(ctx, "dyna:ml the grid has no parameters");
        goto done;
    }
    lens = (size_t*)malloc(nkey * sizeof(size_t));
    pos = (size_t*)malloc(nkey * sizeof(size_t));
    scores = (double*)malloc((nfold ? nfold : 1) * sizeof(double));
    if (!lens || !pos || !scores) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    for (k = 0; k < nkey; k++) {
        JSValue kn = JS_GetPropertyUint32(ctx, keys, (uint32_t)k);
        const char* kname = JS_IsException(kn) ? NULL : JS_ToCString(ctx, kn);
        JSValue vals = kname ? JS_GetPropertyStr(ctx, argv[3], kname) : JS_EXCEPTION;
        JS_FreeCString(ctx, kname);
        JS_FreeValue(ctx, kn);
        if (JS_IsException(vals))
            goto done;
        if (!JS_IsArray(ctx, vals) || dyn_ml_len(ctx, vals, &lens[k])) {
            JS_FreeValue(ctx, vals);
            JS_ThrowTypeError(ctx, "dyna:ml every grid value must be an array");
            goto done;
        }
        JS_FreeValue(ctx, vals);
        if (lens[k] == 0) {
            JS_ThrowTypeError(ctx, "dyna:ml a grid parameter has no values");
            goto done;
        }
        if (total > (size_t)1 << 20 || lens[k] > (size_t)1 << 20
            || (uint64_t)total * lens[k] > (uint64_t)1 << 20) {
            JS_ThrowRangeError(ctx, "dyna:ml the grid has more than 2^20 points");
            goto done;
        }
        total *= lens[k];
        pos[k] = 0;
    }
    if (random && (size_t)n_iter < total)
        total = (size_t)n_iter;

    results = JS_NewArray(ctx);
    if (JS_IsException(results))
        goto done;
    for (combo = 0; combo < total; combo++) {
        JSValue params = JS_NewObject(ctx), rec, sarr;
        double mean = 0.0;
        size_t rest = combo;
        if (JS_IsException(params))
            goto done;
        for (k = 0; k < nkey; k++) {
            JSValue kn = JS_GetPropertyUint32(ctx, keys, (uint32_t)k);
            const char* kname = JS_ToCString(ctx, kn);
            JSValue vals, one;
            size_t pick;
            JS_FreeValue(ctx, kn);
            if (!kname) {
                JS_FreeValue(ctx, params);
                goto done;
            }
            if (random) {
                pick = (size_t)(dyn_splitmix64(&seed) % (uint64_t)lens[k]);
            } else {
                pick = rest % lens[k];
                rest /= lens[k];
            }
            vals = JS_GetPropertyStr(ctx, argv[3], kname);
            one = JS_IsException(vals) ? JS_EXCEPTION
                                       : JS_GetPropertyUint32(ctx, vals, (uint32_t)pick);
            JS_FreeValue(ctx, vals);
            if (JS_IsException(one) || JS_DefinePropertyValueStr(ctx, params, kname, one, JS_PROP_C_W_E) < 0) {
                JS_FreeCString(ctx, kname);
                JS_FreeValue(ctx, params);
                goto done;
            }
            JS_FreeCString(ctx, kname);
        }
        for (f = 0; f < nfold; f++) {
            JSValue fold = JS_GetPropertyUint32(ctx, folds, (uint32_t)f);
            JSValue trv, tev;
            uint32_t *tr = NULL, *te = NULL;
            size_t ntr = 0, nte = 0;
            int bad;
            if (JS_IsException(fold)) {
                JS_FreeValue(ctx, params);
                goto done;
            }
            trv = JS_GetPropertyStr(ctx, fold, "train");
            tev = JS_GetPropertyStr(ctx, fold, "test");
            JS_FreeValue(ctx, fold);
            bad = JS_IsException(trv) || JS_IsException(tev) || dyn_ml_read_idx(ctx, trv, &tr, &ntr, mx.rows) || dyn_ml_read_idx(ctx, tev, &te, &nte, mx.rows);
            JS_FreeValue(ctx, trv);
            JS_FreeValue(ctx, tev);
            if (bad) {
                free(tr);
                free(te);
                JS_FreeValue(ctx, params);
                goto done;
            }
            {
                JSValue m = JS_Call(ctx, argv[0], JS_UNDEFINED, 1,
                    (JSValueConst*)&params);
                JSValue fit, predict, Xtr, ytr, Xte, yte, pred, r;
                JSValueConst a2[2];
                int ok = 0;
                if (JS_IsException(m)) {
                    free(tr);
                    free(te);
                    JS_FreeValue(ctx, params);
                    goto done;
                }
                if (!JS_IsObject(m)) {
                    JS_FreeValue(ctx, m);
                    JS_ThrowTypeError(ctx,
                        "dyna:ml estimator must be a factory returning a model, "
                        "e.g. () => new LogisticRegression()");
                    free(tr);
                    free(te);
                    JS_FreeValue(ctx, params);
                    goto done;
                }
                Xtr = dyn_ml_rows_subset(ctx, &mx, tr, ntr);
                ytr = dyn_ml_vec_subset(ctx, y, tr, ntr);
                Xte = dyn_ml_rows_subset(ctx, &mx, te, nte);
                yte = dyn_ml_vec_subset(ctx, y, te, nte);
                if (JS_IsException(Xtr) || JS_IsException(ytr) || JS_IsException(Xte) || JS_IsException(yte)) {
                    JS_FreeValue(ctx, m);
                    JS_FreeValue(ctx, Xtr);
                    JS_FreeValue(ctx, ytr);
                    JS_FreeValue(ctx, Xte);
                    JS_FreeValue(ctx, yte);
                    free(tr);
                    free(te);
                    JS_FreeValue(ctx, params);
                    goto done;
                }
                fit = JS_GetPropertyStr(ctx, m, "fit");
                predict = JS_GetPropertyStr(ctx, m, "predict");
                if (JS_IsException(fit) || JS_IsException(predict) || !JS_IsFunction(ctx, fit) || !JS_IsFunction(ctx, predict)) {
                    JS_FreeValue(ctx, fit);
                    JS_FreeValue(ctx, predict);
                    JS_FreeValue(ctx, m);
                    JS_FreeValue(ctx, Xtr);
                    JS_FreeValue(ctx, ytr);
                    JS_FreeValue(ctx, Xte);
                    JS_FreeValue(ctx, yte);
                    JS_ThrowTypeError(ctx, "dyna:ml the estimator needs fit() and predict()");
                    free(tr);
                    free(te);
                    JS_FreeValue(ctx, params);
                    goto done;
                }
                a2[0] = Xtr;
                a2[1] = ytr;
                r = JS_Call(ctx, fit, m, 2, a2);
                pred = JS_IsException(r) ? JS_EXCEPTION
                                         : JS_Call(ctx, predict, m, 1, (JSValueConst*)&Xte);
                JS_FreeValue(ctx, r);
                if (!JS_IsException(pred) && !dyn_ml_apply_scorer(ctx, scorer, yte, pred, &scores[f]))
                    ok = 1;
                JS_FreeValue(ctx, pred);
                JS_FreeValue(ctx, fit);
                JS_FreeValue(ctx, predict);
                JS_FreeValue(ctx, Xtr);
                JS_FreeValue(ctx, ytr);
                JS_FreeValue(ctx, Xte);
                JS_FreeValue(ctx, yte);
                if (!JS_IsException(pred)) {
                    JSValue close = JS_GetPropertyStr(ctx, m, "close");
                    if (JS_IsFunction(ctx, close)) {
                        JSValue cr = JS_Call(ctx, close, m, 0, NULL);
                        if (JS_IsException(cr))
                            ok = 0;
                        JS_FreeValue(ctx, cr);
                    }
                    JS_FreeValue(ctx, close);
                }
                JS_FreeValue(ctx, m);
                free(tr);
                free(te);
                if (!ok) {
                    JS_FreeValue(ctx, params);
                    goto done;
                }
                if (dyn_ml_progress(ctx, onprogress, ++work,
                        (size_t)total * nfold)) {
                    JS_FreeValue(ctx, params);
                    goto done;
                }
            }
            mean += scores[f];
        }
        mean /= (double)(nfold ? nfold : 1);
        rec = JS_NewObject(ctx);
        sarr = dyn_ml_doubles_to_js(ctx, scores, nfold);
        if (JS_IsException(rec) || JS_IsException(sarr) || JS_DefinePropertyValueStr(ctx, rec, "params", JS_DupValue(ctx, params), JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueStr(ctx, rec, "scores", sarr, JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueStr(ctx, rec, "mean", JS_NewFloat64(ctx, mean), JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueUint32(ctx, results, (uint32_t)combo, rec, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, params);
            goto done;
        }
        if (mean > bestScore) {
            bestScore = mean;
            JS_FreeValue(ctx, best);
            best = JS_DupValue(ctx, params);
        }
        JS_FreeValue(ctx, params);
    }
    out = JS_NewObject(ctx);
    if (JS_IsException(out))
        goto done;
    if (JS_DefinePropertyValueStr(ctx, out, "best", JS_DupValue(ctx, best),
            JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, out, "bestScore",
               JS_NewFloat64(ctx, bestScore), JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, out, "results", JS_DupValue(ctx, results),
               JS_PROP_C_W_E)
            < 0) {
        JS_FreeValue(ctx, out);
        out = JS_EXCEPTION;
    }
done:
    dyn_matrix_free(&mx);
    free(y);
    free(scores);
    free(lens);
    free(pos);
    JS_FreeValue(ctx, folds);
    JS_FreeValue(ctx, scorer);
    JS_FreeValue(ctx, results);
    JS_FreeValue(ctx, best);
    JS_FreeValue(ctx, keys);
    JS_FreeValue(ctx, onprogress);
    return out;
}

#endif
