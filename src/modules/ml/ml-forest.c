#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

static const char* const ml_tree_keys[] = { "nEstimators", "maxDepth",
    "minSamplesSplit",
    "minSamplesLeaf", "maxFeatures",
    "maxBins", "seed", "learningRate",
    "subsample" };

static const char* const ml_tree_xgb_extra_keys[] = { "lambda", "alpha",
    "gamma",
    "minChildWeight",
    "colsampleByTree",
    "validationFraction",
    "earlyStoppingRounds" };

JSValue dyn_forest_new(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv,
    JSClassID class_id, int classifier, int boosting,
    size_t default_trees, size_t default_depth,
    int newton)
{
    dyn_forest_t* m;
    size_t n_trees = default_trees, seed_holder;
    dyn_tree_opts_t opt;
    double lr = newton ? 0.3 : 0.1, subsample = 1.0, colsample = 1.0;
    size_t early_stop = 0;
    double val_frac = 0.1;
    uint64_t seed = 12345;

    memset(&opt, 0, sizeof(opt));
    opt.max_depth = default_depth;
    opt.min_samples_split = 2;
    opt.min_samples_leaf = 1;
    opt.max_features = 0;
    opt.classifier = classifier;
    opt.newton = newton;
    if (newton) {
        opt.max_bins = DYN_HIST_MAX_BINS;
        opt.lambda = 1.0;
        opt.alpha = 0.0;
        opt.gamma = 0.0;
        opt.min_child_weight = 1.0;
    }

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst o = argv[0];
        const char* ks[16];
        int nk = 0, kt;
        for (kt = 0; kt < 9; kt++)
            ks[nk++] = ml_tree_keys[kt];
        if (newton) {
            for (kt = 0; kt < 7; kt++)
                ks[nk++] = ml_tree_xgb_extra_keys[kt];
        } else {
            ks[nk++] = "validationFraction";
            ks[nk++] = "earlyStoppingRounds";
        }
        if (dyn_opts_strict(ctx, o, ks, nk))
            return JS_EXCEPTION;
        seed_holder = (size_t)seed;
        if (newton && (dyn_opt_double(ctx, o, "lambda", &opt.lambda, 0.0, 1e12) || dyn_opt_double(ctx, o, "alpha", &opt.alpha, 0.0, 1e12) || dyn_opt_double(ctx, o, "gamma", &opt.gamma, 0.0, 1e12) || dyn_opt_double(ctx, o, "minChildWeight", &opt.min_child_weight, 0.0, 1e12) || dyn_opt_double(ctx, o, "colsampleByTree", &colsample, 1e-6, 1.0) || dyn_opt_double(ctx, o, "validationFraction", &val_frac, 0.0, 0.5) || dyn_opt_size(ctx, o, "earlyStoppingRounds", &early_stop, 0)))
            return JS_EXCEPTION;
        if (!newton) {
            JSValue v = JS_GetPropertyStr(ctx, o, "earlyStoppingRounds");
            if (JS_IsException(v))
                return JS_EXCEPTION;
            if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx,
                    "dyna:ml earlyStoppingRounds is only supported by the XGB models");
            }
            JS_FreeValue(ctx, v);
            v = JS_GetPropertyStr(ctx, o, "validationFraction");
            if (JS_IsException(v))
                return JS_EXCEPTION;
            if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx,
                    "dyna:ml validationFraction is only supported by the XGB models");
            }
            JS_FreeValue(ctx, v);
        }
        if (dyn_opt_size(ctx, o, "nEstimators", &n_trees, 1) || dyn_opt_size(ctx, o, "maxDepth", &opt.max_depth, 0) || dyn_opt_size(ctx, o, "minSamplesSplit", &opt.min_samples_split, 2) || dyn_opt_size(ctx, o, "minSamplesLeaf", &opt.min_samples_leaf, 1) || dyn_opt_size(ctx, o, "maxFeatures", &opt.max_features, 0) || dyn_opt_size(ctx, o, "maxBins", &opt.max_bins, 0) || dyn_opt_size(ctx, o, "seed", &seed_holder, 0) || dyn_opt_double(ctx, o, "learningRate", &lr, 1e-12, 1e12) || dyn_opt_double(ctx, o, "subsample", &subsample, 1e-6, 1.0))
            return JS_EXCEPTION;
        seed = (uint64_t)seed_holder;
        if (opt.max_bins && (opt.max_bins < 2 || opt.max_bins > DYN_HIST_MAX_BINS))
            return JS_ThrowRangeError(ctx, "dyna:ml maxBins must be 0 (exact splits) "
                                           "or between 2 and %u",
                (unsigned)DYN_HIST_MAX_BINS);
        if (newton && opt.max_bins == 0)
            return JS_ThrowRangeError(ctx, "dyna:ml maxBins 0 (exact splits) is not "
                                           "supported with the Newton objective");
    } else if (argc > 0 && !JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "dyna:ml expected an options object");
    }
    if (!boosting)
        n_trees = (default_trees == 1) ? 1 : n_trees;
    if (n_trees > DYN_ML_MAX_TREES)
        return JS_ThrowRangeError(ctx, "dyna:ml nEstimators must be at most %u",
            (unsigned)DYN_ML_MAX_TREES);
    if (opt.max_depth > DYN_TREE_MAX_DEPTH)
        return JS_ThrowRangeError(ctx,
            "dyna:ml maxDepth must be at most %u; a deeper tree is grown by "
            "recursion and would exhaust the C stack",
            (unsigned)DYN_TREE_MAX_DEPTH);

    m = (dyn_forest_t*)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->classifier = classifier;
    m->boosting = boosting;
    m->n_trees = m->n_rounds = (default_trees == 1 && !boosting) ? 1 : n_trees;
    m->opt = opt;
    m->lr = lr;
    m->subsample = subsample;
    m->colsample = colsample;
    m->early_stop = early_stop;
    m->val_frac = val_frac;
    m->seed = seed;
    return dyn_res_wrap(ctx, new_target, class_id, m, dyn_forest_dispose);
}

static int dyn_forest_learn(JSContext* ctx, dyn_forest_t* m, const double* X,
    const double* y, size_t rows, size_t cols,
    const double* w)
{
    dyn_tree_t* trees = NULL;
    uint32_t *idx = NULL, *scratch = NULL, *feat = NULL, *train = NULL;
    uint8_t* isval = NULL;
    dyn_tree_kv_t* kv = NULL;
    uint32_t* best_order = NULL;
    double *cnt_l = NULL, *cnt_r = NULL, *cnt_tot = NULL, *classes = NULL;
    double *resid = NULL, *pred = NULL, *hess = NULL, *raw_base = NULL;
    double *lnum = NULL, *lden = NULL, *hbuf = NULL, *binvals = NULL;
    size_t lnum_cap = 0;
    uint32_t* hnext = NULL;
    uint8_t* cls_of = NULL;
    size_t n_classes = 1, i, ti, use_n, n_out = 1, n_alloc, rounds, n_keep;
    uint64_t rng = m->seed;
    dyn_tree_opts_t opt = m->opt;
    dyn_hist_t hist = { 0 };
    dyn_tree_work_t work = { 0 };
    int have_hist = 0, rc = -1;

    if (m->classifier) {
        n_classes = dyn_ml_classes(ctx, y, rows, &classes);
        if (n_classes == 0)
            return -1;
        if (n_classes > 256) {
            free(classes);
            JS_ThrowRangeError(ctx,
                "ml.fit: tree models support at most 256 classes, found %u",
                (unsigned)n_classes);
            return -1;
        }
    }
    rounds = m->n_rounds;
    if (m->boosting && m->classifier) {
        if (n_classes < 2) {
            free(classes);
            JS_ThrowTypeError(ctx, "ml.fit: %s needs at least two classes in y, "
                                   "found %u",
                opt.newton ? "XGBClassifier"
                           : "GradientBoostingClassifier",
                (unsigned)n_classes);
            return -1;
        }
        n_out = (n_classes == 2) ? 1 : n_classes;
    }
    if (m->boosting && n_out && rounds > SIZE_MAX / n_out) {
        JS_ThrowRangeError(ctx, "ml.fit: allocation size overflow");
        goto done;
    }
    n_alloc = m->boosting ? rounds * n_out : rounds;
    if (n_alloc > SIZE_MAX / sizeof(dyn_tree_t) || rows > SIZE_MAX / sizeof(dyn_tree_kv_t) || rows > SIZE_MAX / sizeof(uint32_t) || cols > SIZE_MAX / sizeof(uint32_t) || n_classes > SIZE_MAX / sizeof(double)) {
        JS_ThrowRangeError(ctx, "ml.fit: allocation size overflow");
        goto done;
    }
    n_keep = n_alloc;
    trees = (dyn_tree_t*)calloc(n_alloc ? n_alloc : 1, sizeof(*trees));
    idx = (uint32_t*)malloc(rows * sizeof(uint32_t));
    scratch = (uint32_t*)malloc(rows * sizeof(uint32_t));
    kv = (dyn_tree_kv_t*)malloc(rows * sizeof(dyn_tree_kv_t));
    best_order = (uint32_t*)malloc(rows * sizeof(uint32_t));
    feat = (uint32_t*)malloc(cols * sizeof(uint32_t));
    cnt_l = (double*)malloc(n_classes * sizeof(double));
    cnt_r = (double*)malloc(n_classes * sizeof(double));
    cnt_tot = (double*)malloc(n_classes * sizeof(double));
    if (opt.classifier && classes) {
        cls_of = (uint8_t*)malloc(rows ? rows : 1);
        if (cls_of) {
            for (i = 0; i < rows; i++) {
                size_t kk;
                cls_of[i] = 0;
                for (kk = 0; kk < n_classes; kk++)
                    if (classes[kk] == y[i]) {
                        cls_of[i] = (uint8_t)kk;
                        break;
                    }
            }
        }
    }
    if (m->boosting) {
        if (rows > (SIZE_MAX / sizeof(double)) / n_out) {
            JS_ThrowRangeError(ctx, "ml.fit: data too large for boosting");
            goto done;
        }
        resid = (double*)malloc(rows * sizeof(double));
        pred = (double*)malloc(rows * n_out * sizeof(double));
        raw_base = (double*)calloc(n_out, sizeof(double));
        if (m->classifier || opt.newton)
            hess = (double*)malloc(rows * sizeof(double));
    }
    if (opt.newton) {
        isval = (uint8_t*)calloc(rows, sizeof(uint8_t));
        train = (uint32_t*)malloc(rows * sizeof(uint32_t));
    }
    if (!trees || !idx || !scratch || !kv || !best_order || !feat || !cnt_l || !cnt_r || !cnt_tot || (opt.classifier && classes && !cls_of) || (opt.newton && (!isval || !train)) || (m->boosting && (!resid || !pred || !raw_base)) || (m->boosting && (m->classifier || opt.newton) && !hess)) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    memset(&work, 0, sizeof(work));
    work.w = opt.newton ? NULL : w;
    work.cls_of = cls_of;
    work.scratch = scratch;
    work.feat = feat;
    work.cnt_l = cnt_l;
    work.cnt_r = cnt_r;
    work.cnt_tot = cnt_tot;
    work.kv = kv;
    work.best_order = best_order;
    if (!opt.max_bins)
        work.tiefree = (uint8_t*)calloc(cols, 1);
    if (opt.max_bins) {
        size_t payload = (opt.classifier && n_classes > 2) ? n_classes : 2;
        size_t stride = 1 + payload + ((w && !opt.newton) ? 1 : 0);
        binvals = (double*)malloc(rows * sizeof(double));
        if (!binvals) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        if (dyn_hist_build(&hist, X, rows, cols, opt.max_bins, kv, binvals,
                opt.newton ? NULL : w)) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        have_hist = 1;
        hbuf = (double*)calloc((hist.max_bins + 1) * stride, sizeof(double));
        hnext = (uint32_t*)malloc((hist.max_bins + 2) * sizeof(uint32_t));
        if (!hbuf || !hnext) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        work.hist = &hist;
        work.hbuf = hbuf;
        work.hnext = hnext;
        work.hstride = stride;
    }
    if (opt.max_features == 0 && m->n_rounds > 1 && !m->boosting) {
        double r = m->classifier ? sqrt((double)cols) : (double)cols / 3.0;
        size_t v = (size_t)(r + 0.5);
        opt.max_features = v ? v : 1;
    }

    if (opt.newton) {
        dyn_tree_opts_t topt = opt;
        size_t r, k, ntrain, nval = 0, kept = rounds, since = 0, best_round = 0;
        double best_loss = 0.0;
        int have_best = 0;

        topt.classifier = 0;
        dyn_boost_init_base(m->classifier, n_out, y, rows, classes, w,
            raw_base, pred);

        if (m->early_stop && m->val_frac > 0.0 && rows >= 4) {
            nval = (size_t)((double)rows * m->val_frac + 0.5);
            if (nval < 1)
                nval = 1;
            if (nval > rows / 2)
                nval = rows / 2;
            for (i = 0; i < rows; i++)
                idx[i] = (uint32_t)i;
            for (i = 0; i < nval; i++) {
                size_t j = i + (size_t)(dyn_splitmix64(&rng) % (uint64_t)(rows - i));
                uint32_t tmp = idx[i];
                idx[i] = idx[j];
                idx[j] = tmp;
            }
            for (i = 0; i < nval; i++)
                isval[idx[i]] = 1;
        }
        ntrain = 0;
        for (i = 0; i < rows; i++)
            if (!isval[i])
                train[ntrain++] = (uint32_t)i;

        use_n = (size_t)((double)ntrain * m->subsample + 0.5);
        if (use_n < 1)
            use_n = 1;
        if (use_n > ntrain)
            use_n = ntrain;

        for (r = 0; r < rounds; r++) {
            memcpy(idx, train, ntrain * sizeof(uint32_t));
            if (use_n < ntrain) {
                for (i = 0; i < use_n; i++) {
                    size_t j = i + (size_t)(dyn_splitmix64(&rng) % (uint64_t)(ntrain - i));
                    uint32_t tmp = idx[i];
                    idx[i] = idx[j];
                    idx[j] = tmp;
                }
            }
            for (k = 0; k < n_out; k++) {
                dyn_tree_t* tr = &trees[r * n_out + k];
                size_t ncand = (size_t)((double)cols * m->colsample + 0.5);
                if (ncand < 1)
                    ncand = 1;
                if (ncand > cols)
                    ncand = cols;
                for (i = 0; i < cols; i++)
                    feat[i] = (uint32_t)i;
                if (ncand < cols) {
                    for (i = 0; i < ncand; i++) {
                        size_t j = i + (size_t)(dyn_splitmix64(&rng) % (uint64_t)(cols - i));
                        uint32_t tmp = feat[i];
                        feat[i] = feat[j];
                        feat[j] = tmp;
                    }
                }
                work.n_cand = ncand;
                dyn_xgb_grad_hess(m->classifier, n_out, k, pred, y, classes, w,
                    rows, resid, hess);
                work.grad = resid;
                work.hess = hess;
                if (dyn_tree_fit_one(ctx, tr, X, resid, rows, cols, NULL, 1,
                        &topt, &rng, idx, use_n, &work))
                    goto done;
                for (i = 0; i < rows; i++)
                    pred[i * n_out + k] += m->lr * dyn_tree_predict_row_missing(tr, X + i * cols);
            }
            if (nval) {
                double loss = dyn_xgb_val_loss(m->classifier, n_out, pred, y,
                    classes, isval, rows);
                if (!have_best || loss < best_loss - 1e-12) {
                    best_loss = loss;
                    best_round = r + 1;
                    have_best = 1;
                    since = 0;
                } else if (++since >= m->early_stop) {
                    break;
                }
            }
        }
        if (have_best)
            kept = best_round;
        n_keep = kept * n_out;
        m->best_rounds = kept;
    } else if (m->boosting) {
        dyn_tree_opts_t topt = opt;
        size_t r, k;
        topt.classifier = 0;
        dyn_boost_init_base(m->classifier, n_out, y, rows, classes, w,
            raw_base, pred);
        use_n = (size_t)((double)rows * m->subsample + 0.5);
        if (use_n < 1)
            use_n = 1;
        if (use_n > rows)
            use_n = rows;
        for (r = 0; r < rounds; r++) {
            for (i = 0; i < rows; i++)
                idx[i] = (uint32_t)i;
            if (use_n < rows) {
                for (i = 0; i < use_n; i++) {
                    size_t j = i + (size_t)(dyn_splitmix64(&rng) % (uint64_t)(rows - i));
                    uint32_t t = idx[i];
                    idx[i] = idx[j];
                    idx[j] = t;
                }
            }
            for (k = 0; k < n_out; k++) {
                dyn_tree_t* tr = &trees[r * n_out + k];
                double scale = 1.0;
                if (!m->classifier) {
                    for (i = 0; i < rows; i++)
                        resid[i] = y[i] - pred[i];
                } else if (n_out == 1) {
                    for (i = 0; i < rows; i++) {
                        double p = 1.0 / (1.0 + exp(-pred[i]));
                        resid[i] = ((y[i] == classes[1]) ? 1.0 : 0.0) - p;
                        hess[i] = p * (1.0 - p);
                    }
                } else {
                    for (i = 0; i < rows; i++) {
                        const double* f = pred + i * n_out;
                        double mx = f[0], s = 0.0, p;
                        size_t j;
                        for (j = 1; j < n_out; j++)
                            if (f[j] > mx)
                                mx = f[j];
                        for (j = 0; j < n_out; j++)
                            s += exp(f[j] - mx);
                        p = exp(f[k] - mx) / s;
                        resid[i] = ((y[i] == classes[k]) ? 1.0 : 0.0) - p;
                        hess[i] = fabs(resid[i]) * (1.0 - fabs(resid[i]));
                    }
                    scale = (double)(n_out - 1) / (double)n_out;
                }
                if (dyn_tree_fit_one(ctx, tr, X, resid, rows, cols, NULL, 1,
                        &topt, &rng, idx, use_n, &work))
                    goto done;
                if (m->classifier) {
                    if (tr->n_nodes > lnum_cap) {
                        double *nn, *dd;
                        size_t want = lnum_cap ? lnum_cap : 16;
                        while (want < tr->n_nodes && want <= SIZE_MAX / 16)
                            want *= 2;
                        if (want < tr->n_nodes)
                            want = tr->n_nodes;
                        nn = (double*)realloc(lnum, want * sizeof(double));
                        if (!nn) {
                            JS_ThrowOutOfMemory(ctx);
                            goto done;
                        }
                        lnum = nn;
                        dd = (double*)realloc(lden, want * sizeof(double));
                        if (!dd) {
                            JS_ThrowOutOfMemory(ctx);
                            goto done;
                        }
                        lden = dd;
                        lnum_cap = want;
                    }
                    dyn_boost_leaf_newton(tr, X, cols, resid, hess, w, idx,
                        use_n, scale, lnum, lden);
                }
                for (i = 0; i < rows; i++)
                    pred[i * n_out + k] += m->lr * dyn_tree_predict_row(tr, X + i * cols);
            }
        }
    } else {
        for (ti = 0; ti < rounds; ti++) {
            if (rounds == 1) {
                for (i = 0; i < rows; i++)
                    idx[i] = (uint32_t)i;
            } else {
                uint64_t M = dyn_fastmod_M((uint64_t)rows);
                for (i = 0; i < rows; i++) {
                    uint64_t draw = dyn_splitmix64(&rng);
                    idx[i] = (uint32_t)(rows == 1 ? 0
                                                  : dyn_fastmod(draw, M, (uint64_t)rows));
                }
            }
            if (dyn_tree_fit_one(ctx, &trees[ti], X, y, rows, cols, classes,
                    n_classes, &opt, &rng, idx, rows, &work))
                goto done;
        }
    }
    if (m->trees) {
        for (i = 0; i < m->n_trees; i++)
            dyn_tree_free(&m->trees[i]);
        free(m->trees);
    }
    free(m->classes);
    free(m->raw_base);
    for (i = n_keep; i < n_alloc; i++)
        dyn_tree_free(&trees[i]);
    m->trees = trees;
    m->classes = classes;
    m->n_classes = n_classes;
    m->n_features = cols;
    m->n_trees = n_keep;
    m->n_out = m->boosting ? n_out : 0;
    m->raw_base = raw_base;
    m->fitted = 1;
    m->opt.max_features = opt.max_features;
    if (!opt.newton)
        m->best_rounds = rounds;
    trees = NULL;
    classes = NULL;
    raw_base = NULL;
    rc = 0;
done:
    if (trees) {
        for (i = 0; i < n_alloc; i++)
            dyn_tree_free(&trees[i]);
        free(trees);
    }
    free(classes);
    free(idx);
    free(scratch);
    free(kv);
    free(best_order);
    free(feat);
    free(cnt_l);
    free(cnt_r);
    free(cnt_tot);
    free(resid);
    free(pred);
    free(hess);
    free(raw_base);
    free(lnum);
    free(lden);
    free(hbuf);
    free(binvals);
    free(hnext);
    free(isval);
    free(train);
    free(cls_of);
    free(work.tiefree);
    if (have_hist)
        dyn_hist_free(&hist);
    return rc;
}

JSValue dyn_forest_fit(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, JSClassID class_id,
    int newton)
{
    dyn_forest_t* m;
    dyn_matrix_t mx = { 0 };
    double *y = NULL, *w = NULL;
    JSValueConst rows_arg, cols_arg;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ml.fit: fit(X, y) requires two arguments");
    rows_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    cols_arg = argc > 3 ? argv[3] : JS_UNDEFINED;
    if (dyn_ml_ingest_Xy_ex(ctx, argv[0], argv[1], rows_arg, cols_arg,
            argc, argv, 2, &mx, &y, &w, newton))
        return JS_EXCEPTION;
    if (mx.cols > (size_t)INT32_MAX) {
        dyn_matrix_free(&mx);
        free(y);
        free(w);
        return JS_ThrowRangeError(ctx, "ml.fit: too many columns for a tree model");
    }
    m = (dyn_forest_t*)dyn_res_native(ctx, this_val, class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        free(y);
        free(w);
        return JS_EXCEPTION;
    }
    dyn_fiber_offload();
    if (dyn_forest_learn(ctx, m, mx.data, y, mx.rows, mx.cols, w)) {
        dyn_matrix_free(&mx);
        free(y);
        free(w);
        return JS_EXCEPTION;
    }
    free(w);
    dyn_matrix_free(&mx);
    free(y);
    return JS_DupValue(ctx, this_val);
}

static double dyn_forest_raw_score(const dyn_forest_t* m, const double* x,
    size_t k)
{
    double acc = m->raw_base[k];
    size_t r, rounds = m->n_out ? m->n_trees / m->n_out : 0;
    if (m->opt.newton) {
        for (r = 0; r < rounds; r++)
            acc += m->lr * dyn_tree_predict_row_missing(&m->trees[r * m->n_out + k], x);
        return acc;
    }
    for (r = 0; r < rounds; r++)
        acc += m->lr * dyn_tree_predict_row(&m->trees[r * m->n_out + k], x);
    return acc;
}

static JSValue dyn_forest_predict_impl(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    JSClassID class_id, int into)
{
    dyn_forest_t* m;
    dyn_matrix_t mx = { 0 };
    dyn_csr_t* S = NULL;
    dyn_ml_row_src rs = { 0 };
    double *out = NULL, *votes = NULL;
    size_t rows, i, ti, c;
    int as_f64 = 0;
    JSValue result;
    JSValueConst xa, rows_arg, cols_arg;

    dyn_ml_into_args(into, argc, argv, &xa, &rows_arg, &cols_arg);
    if (!into && dyn_ml_parse_as_opt(ctx, argc, argv, 1, &as_f64))
        return JS_EXCEPTION;
    if (JS_GetClassID(xa) == dyn_csr_class_id) {
        S = dyn_ml_as_csr(ctx, xa);
        if (!S)
            return JS_EXCEPTION;
    } else if (dyn_ml_ingest_X(ctx, xa, rows_arg, cols_arg, &mx)) {
        return JS_EXCEPTION;
    }
    m = (dyn_forest_t*)dyn_res_native(ctx, this_val, class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return dyn_ml_throw_unfitted(ctx, this_val, "predict");
    }
    if ((S ? S->cols : mx.cols) != m->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "ml.predict: X has %llu features, model expects %llu",
            (unsigned long long)(S ? S->cols : mx.cols),
            (unsigned long long)m->n_features);
    }
    rows = S ? S->rows : mx.rows;
    if (rows > SIZE_MAX / sizeof(double)) {
        dyn_matrix_free(&mx);
        return JS_ThrowRangeError(ctx, "ml.predict: allocation size overflow");
    }
    if (into) {
        if (dyn_ml_bind_out(ctx, argv[0], rows, &out)) {
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
    } else {
        out = (double*)malloc(rows * sizeof(double));
    }
    if (m->classifier && !votes)
        votes = (double*)calloc(m->n_classes, sizeof(double));
    if (!out || (m->classifier && !votes)) {
        if (!into)
            free(out);
        free(votes);
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    dyn_fiber_offload();
    if (dyn_ml_row_src_init(ctx, &rs, &mx, S)) {
        if (!into)
            free(out);
        free(votes);
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    for (i = 0; i < rows; i++) {
        const double* xi = dyn_ml_row_at(&rs, i);
        if (m->boosting && m->classifier) {
            size_t k, best = 0;
            double bestf = 0.0;
            for (k = 0; k < m->n_out; k++) {
                double f = dyn_forest_raw_score(m, xi, k);
                if (k == 0 || f > bestf) {
                    bestf = f;
                    best = k;
                }
            }
            out[i] = (m->n_out == 1) ? m->classes[bestf > 0.0 ? 1 : 0]
                                     : m->classes[best];
        } else if (m->boosting) {
            double acc = m->raw_base[0];
            for (ti = 0; ti < m->n_trees; ti++)
                acc += m->lr * dyn_tree_predict_row(&m->trees[ti], xi);
            out[i] = acc;
        } else if (!m->classifier) {
            double acc = 0.0;
            for (ti = 0; ti < m->n_trees; ti++)
                acc += dyn_tree_predict_row(&m->trees[ti], xi);
            out[i] = acc / (double)m->n_trees;
        } else {
            size_t best = 0;
            memset(votes, 0, m->n_classes * sizeof(double));
            for (ti = 0; ti < m->n_trees; ti++) {
                const dyn_tree_t* t = &m->trees[ti];
                uint32_t leaf;
                if (t->n_classes != m->n_classes || !t->proba) {
                    double lab = dyn_tree_predict_row(t, xi);
                    for (c = 0; c < m->n_classes; c++)
                        if (m->classes[c] == lab) {
                            votes[c] += 1.0;
                            break;
                        }
                    continue;
                }
                leaf = dyn_tree_leaf_index(t, xi, rs.cols);
                for (c = 0; c < m->n_classes; c++)
                    votes[c] += t->proba[(size_t)leaf * m->n_classes + c];
            }
            for (c = 1; c < m->n_classes; c++)
                if (votes[c] > votes[best])
                    best = c;
            out[i] = m->classes[best];
        }
    }
    dyn_ml_row_src_free(&rs);
    dyn_matrix_free(&mx);
    free(votes);
    if (into)
        return JS_NewInt64(ctx, (int64_t)rows);
    result = dyn_ml_predict_result(ctx, out, rows, as_f64);
    if (!into)
        free(out);
    return result;
}

JSValue dyn_forest_predict(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    JSClassID class_id)
{
    return dyn_forest_predict_impl(ctx, this_val, argc, argv, class_id, 0);
}

JSValue dyn_forest_predict_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    JSClassID class_id)
{
    return dyn_forest_predict_impl(ctx, this_val, argc, argv, class_id, 1);
}

JSValue dyn_forest_predict_proba(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    JSClassID class_id)
{
    dyn_forest_t* m;
    dyn_matrix_t mx = { 0 };
    dyn_csr_t* S = NULL;
    dyn_ml_row_src rs = { 0 };
    double* out = NULL;
    size_t rows, i, ti, c, k;
    JSValue result;
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    if (JS_GetClassID(argv[0]) == dyn_csr_class_id) {
        S = dyn_ml_as_csr(ctx, argv[0]);
        if (!S)
            return JS_EXCEPTION;
    } else if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx)) {
        return JS_EXCEPTION;
    }
    m = (dyn_forest_t*)dyn_res_native(ctx, this_val, class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return dyn_ml_throw_unfitted(ctx, this_val, "predictProba");
    }
    if (!m->classifier) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx,
            "ml.predictProba: predictProba is for classifiers; this model predicts a number");
    }
    if ((S ? S->cols : mx.cols) != m->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "ml.predictProba: X has %llu features, model expects %llu",
            (unsigned long long)(S ? S->cols : mx.cols),
            (unsigned long long)m->n_features);
    }
    rows = S ? S->rows : mx.rows;
    k = m->n_classes;
    out = (double*)calloc(rows * (k ? k : 1), sizeof(double));
    if (!out) {
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    dyn_fiber_offload();
    if (dyn_ml_row_src_init(ctx, &rs, &mx, S)) {
        free(out);
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    for (i = 0; i < rows; i++) {
        const double* xi = dyn_ml_row_at(&rs, i);
        if (m->boosting) {
            if (m->n_out == 1) {
                double p = 1.0 / (1.0 + exp(-dyn_forest_raw_score(m, xi, 0)));
                out[i * k + 0] = 1.0 - p;
                out[i * k + 1] = p;
            } else {
                double mxf = 0.0, s = 0.0;
                for (c = 0; c < k; c++) {
                    out[i * k + c] = dyn_forest_raw_score(m, xi, c);
                    if (c == 0 || out[i * k + c] > mxf)
                        mxf = out[i * k + c];
                }
                for (c = 0; c < k; c++) {
                    out[i * k + c] = exp(out[i * k + c] - mxf);
                    s += out[i * k + c];
                }
                for (c = 0; c < k; c++)
                    out[i * k + c] /= s;
            }
            continue;
        }
        for (ti = 0; ti < m->n_trees; ti++) {
            const dyn_tree_t* t = &m->trees[ti];
            uint32_t leaf;
            if (t->n_classes != k || !t->proba)
                continue;
            leaf = dyn_tree_leaf_index(t, xi, rs.cols);
            for (c = 0; c < k; c++)
                out[i * k + c] += t->proba[(size_t)leaf * k + c];
        }
        for (c = 0; c < k; c++)
            out[i * k + c] /= (double)m->n_trees;
    }
    dyn_ml_row_src_free(&rs);
    dyn_matrix_free(&mx);
    result = dyn_ml_matrix_to_js(ctx, out, rows, k, 0);
    free(out);
    return result;
}

JSValue dyn_forest_apply(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    JSClassID class_id)
{
    dyn_forest_t* m;
    dyn_matrix_t mx = { 0 };
    double* out;
    size_t rows, i, ti;
    JSValue result;
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    m = (dyn_forest_t*)dyn_res_native(ctx, this_val, class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return dyn_ml_throw_unfitted(ctx, this_val, "apply");
    }
    if (mx.cols != m->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "ml.apply: X has %llu features, model expects %llu",
            (unsigned long long)mx.cols,
            (unsigned long long)m->n_features);
    }
    rows = mx.rows;
    out = (double*)malloc(rows * (m->n_trees ? m->n_trees : 1) * sizeof(double));
    if (!out) {
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    dyn_fiber_offload();
    for (i = 0; i < rows; i++) {
        const double* xi = mx.data + i * mx.cols;
        for (ti = 0; ti < m->n_trees; ti++)
            out[i * m->n_trees + ti] = (double)(m->opt.newton
                    ? dyn_tree_leaf_index_missing(&m->trees[ti], xi)
                    : dyn_tree_leaf_index(&m->trees[ti], xi, mx.cols));
    }
    dyn_matrix_free(&mx);
    result = dyn_ml_matrix_to_js(ctx, out, rows, m->n_trees, 0);
    free(out);
    return result;
}

JSValue dyn_forest_importances(JSContext* ctx, JSValueConst this_val,
    JSClassID class_id)
{
    dyn_forest_t* m = (dyn_forest_t*)dyn_res_native(ctx, this_val, class_id);
    double *imp, total = 0.0;
    size_t ti, i, f;
    JSValue result;

    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return dyn_ml_throw_unfitted(ctx, this_val, "featureImportances");
    imp = (double*)calloc(m->n_features ? m->n_features : 1, sizeof(double));
    if (!imp)
        return JS_ThrowOutOfMemory(ctx);
    for (ti = 0; ti < m->n_trees; ti++) {
        const dyn_tree_t* t = &m->trees[ti];
        double root = t->n_nodes ? t->nsamp[0] : 0.0;
        if (root <= 0.0)
            continue;
        for (i = 0; i < t->n_nodes; i++) {
            f = (size_t)t->node[i].feature;
            if (t->node[i].feature < 0 || f >= m->n_features)
                continue;
            imp[f] += t->improve[i] / root;
        }
    }
    for (f = 0; f < m->n_features; f++)
        total += imp[f];
    if (total > 0.0)
        for (f = 0; f < m->n_features; f++)
            imp[f] /= total;
    result = dyn_ml_doubles_to_js(ctx, imp, m->n_features);
    free(imp);
    return result;
}

JSValue dyn_forest_depth(JSContext* ctx, JSValueConst this_val,
    JSClassID class_id)
{
    dyn_forest_t* m = (dyn_forest_t*)dyn_res_native(ctx, this_val, class_id);
    size_t ti, i, maxd = 0;
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewInt32(ctx, 0);
    for (ti = 0; ti < m->n_trees; ti++) {
        const dyn_tree_t* t = &m->trees[ti];
        uint32_t* d = (uint32_t*)calloc(t->n_nodes ? t->n_nodes : 1,
            sizeof(uint32_t));
        if (!d)
            return JS_ThrowOutOfMemory(ctx);
        for (i = 0; i < t->n_nodes; i++) {
            if (t->node[i].feature < 0) {
                if (d[i] > maxd)
                    maxd = d[i];
                continue;
            }
            d[t->node[i].left] = d[i] + 1;
            d[t->node[i].right] = d[i] + 1;
        }
        free(d);
    }
    return JS_NewInt32(ctx, (int)maxd);
}

#endif
