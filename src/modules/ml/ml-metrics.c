#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

static inline double dyn_ml_sum(const double* x, size_t n)
{
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    size_t j = 0;
#ifndef DYN_ML_NO_SIMD
    for (; j + 4 <= n; j += 4) {
        s0 += x[j];
        s1 += x[j + 1];
        s2 += x[j + 2];
        s3 += x[j + 3];
    }
#endif
    for (; j < n; j++)
        s0 += x[j];
    return (s0 + s1) + (s2 + s3);
}

typedef struct {
    double tp, fp, tn, fn_;
} dyn_binconf;

typedef struct {
    double s;
    size_t i;
} dyn_ap_kv_t;

static int dyn_metric_pair(JSContext* ctx, JSValueConst av, JSValueConst bv,
    double** pa, double** pb, size_t* pn)
{
    size_t n, nb;

    if (dyn_ml_any_len(ctx, av, &n) || dyn_ml_any_len(ctx, bv, &nb))
        return -1;
    if (n == 0) {
        JS_ThrowRangeError(ctx, "dyna:ml metric needs at least one sample");
        return -1;
    }
    if (nb != n) {
        JS_ThrowTypeError(ctx, "dyna:ml y length must equal the number of rows in X");
        return -1;
    }
    if (dyn_ml_ingest_vector(ctx, av, n, pa))
        return -1;
    if (dyn_ml_ingest_vector(ctx, bv, n, pb)) {
        free(*pa);
        *pa = NULL;
        return -1;
    }
    *pn = n;
    return 0;
}

static JSValue dyn_metric_logloss_multi(JSContext* ctx, JSValueConst yv,
    JSValueConst pv)
{
    dyn_matrix_t P = { 0 };
    double *yt = NULL, *classes = NULL;
    size_t n_classes, i, k;
    double acc = 0.0;
    const double EPS = 1e-15;

    if (dyn_ml_ingest_matrix_array(ctx, pv, &P))
        return JS_EXCEPTION;
    if (dyn_ml_ingest_vector(ctx, yv, P.rows, &yt)) {
        dyn_matrix_free(&P);
        return JS_EXCEPTION;
    }
    dyn_fiber_offload();
    for (i = 0; i < P.rows; i++)
        if (!isfinite(yt[i])) {
            dyn_matrix_free(&P);
            free(yt);
            return JS_ThrowRangeError(ctx, "ml.logLoss: logLoss labels must be finite");
        }
    n_classes = dyn_ml_classes(ctx, yt, P.rows, &classes);
    if (n_classes == 0) {
        dyn_matrix_free(&P);
        free(yt);
        return JS_EXCEPTION;
    }
    if (n_classes != P.cols) {
        dyn_matrix_free(&P);
        free(yt);
        free(classes);
        return JS_ThrowTypeError(ctx,
            "ml.logLoss: yPred has %u columns but yTrue names %u distinct labels; "
            "the columns are the classes in ascending label order",
            (unsigned)P.cols, (unsigned)n_classes);
    }
    for (i = 0; i < P.rows; i++) {
        double p = 0.0;
        for (k = 0; k < n_classes; k++)
            if (classes[k] == yt[i])
                p = P.data[i * P.cols + k];
        if (!(p >= EPS))
            p = EPS;
        else if (!(p <= 1.0))
            p = 1.0;
        acc += -log(p);
    }
    {
        double result = P.rows ? acc / (double)P.rows : 0.0;
        dyn_matrix_free(&P);
        free(yt);
        free(classes);
        return JS_NewFloat64(ctx, result);
    }
}

JSValue dyn_metric(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    double *yt = NULL, *yp = NULL;
    size_t n, i;
    double acc = 0.0, result;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:ml expected (yTrue, yPred)");
    if (magic == DYN_METRIC_LOGLOSS && dyn_ml_is_matrix(ctx, argv[1]))
        return dyn_metric_logloss_multi(ctx, argv[0], argv[1]);
    if (dyn_metric_pair(ctx, argv[0], argv[1], &yt, &yp, &n))
        return JS_EXCEPTION;
    dyn_fiber_offload();
    for (i = 0; i < n; i++)
        if (!isfinite(yt[i]) || !isfinite(yp[i])) {
            free(yt);
            free(yp);
            return JS_ThrowRangeError(ctx,
                "dyna:ml metric inputs must be finite");
        }

    switch (magic) {
    case DYN_METRIC_MSE:
        for (i = 0; i < n; i++) {
            double d = yt[i] - yp[i];
            acc += d * d;
        }
        result = acc / (double)n;
        break;
    case DYN_METRIC_MAE:
        for (i = 0; i < n; i++)
            acc += fabs(yt[i] - yp[i]);
        result = acc / (double)n;
        break;
    case DYN_METRIC_R2: {
        double mean = dyn_ml_sum(yt, n) / (double)n;
        double ss_res = 0.0, ss_tot = 0.0;
        for (i = 0; i < n; i++) {
            double dr = yt[i] - yp[i];
            double dt = yt[i] - mean;
            ss_res += dr * dr;
            ss_tot += dt * dt;
        }
        if (ss_tot == 0.0)
            result = (ss_res == 0.0) ? 1.0 : 0.0;
        else
            result = 1.0 - ss_res / ss_tot;
        break;
    }
    case DYN_METRIC_LOGLOSS: {
        const double EPS = 1e-15;
        for (i = 0; i < n; i++) {
            double p = yp[i];
            double t = (yt[i] != 0.0) ? 1.0 : 0.0;
            if (p < EPS)
                p = EPS;
            if (p > 1.0 - EPS)
                p = 1.0 - EPS;
            acc += t ? -log(p) : -log(1.0 - p);
        }
        result = acc / (double)n;
        break;
    }
    default:
        for (i = 0; i < n; i++)
            if (yt[i] == yp[i])
                acc += 1.0;
        result = acc / (double)n;
        break;
    }
    free(yt);
    free(yp);
    return JS_NewFloat64(ctx, result);
}

static dyn_binconf dyn_binconf_of(const double* yt, const double* yp, size_t n,
    double pos)
{
    dyn_binconf c = { 0, 0, 0, 0 };
    size_t i;
    for (i = 0; i < n; i++) {
        int t = (yt[i] == pos), p = (yp[i] == pos);
        if (t && p)
            c.tp += 1.0;
        else if (!t && p)
            c.fp += 1.0;
        else if (!t && !p)
            c.tn += 1.0;
        else
            c.fn_ += 1.0;
    }
    return c;
}

JSValue dyn_metric_binary(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    double *yt = NULL, *yp = NULL, pos = 1.0, r;
    size_t n;
    dyn_binconf c;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:ml expected (yTrue, yPred[, positive])");
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && JS_ToFloat64(ctx, &pos, argv[2]))
        return JS_EXCEPTION;
    if (dyn_metric_pair(ctx, argv[0], argv[1], &yt, &yp, &n))
        return JS_EXCEPTION;
    dyn_fiber_offload();
    c = dyn_binconf_of(yt, yp, n, pos);
    free(yt);
    free(yp);

    switch (magic) {
    case DYN_BM_PRECISION:
        r = (c.tp + c.fp) == 0.0 ? 0.0 : c.tp / (c.tp + c.fp);
        break;
    case DYN_BM_RECALL:
        r = (c.tp + c.fn_) == 0.0 ? 0.0 : c.tp / (c.tp + c.fn_);
        break;
    case DYN_BM_F1: {
        double p = (c.tp + c.fp) == 0.0 ? 0.0 : c.tp / (c.tp + c.fp);
        double q = (c.tp + c.fn_) == 0.0 ? 0.0 : c.tp / (c.tp + c.fn_);
        r = (p + q) == 0.0 ? 0.0 : 2.0 * p * q / (p + q);
        break;
    }
    case DYN_BM_SPECIFICITY:
        r = (c.tn + c.fp) == 0.0 ? 0.0 : c.tn / (c.tn + c.fp);
        break;
    case DYN_BM_BALANCED_ACC: {
        double rec = (c.tp + c.fn_) == 0.0 ? 0.0 : c.tp / (c.tp + c.fn_);
        double spe = (c.tn + c.fp) == 0.0 ? 0.0 : c.tn / (c.tn + c.fp);
        r = 0.5 * (rec + spe);
        break;
    }
    case DYN_BM_MCC: {
        double num = c.tp * c.tn - c.fp * c.fn_;
        double den = sqrt((c.tp + c.fp) * (c.tp + c.fn_) * (c.tn + c.fp) * (c.tn + c.fn_));
        r = (den == 0.0) ? 0.0 : num / den;
        break;
    }
    default: {
        double tot = c.tp + c.fp + c.tn + c.fn_;
        double po, pe;
        if (tot == 0.0) {
            r = 0.0;
            break;
        }
        po = (c.tp + c.tn) / tot;
        pe = ((c.tp + c.fp) * (c.tp + c.fn_) + (c.tn + c.fn_) * (c.tn + c.fp)) / (tot * tot);
        r = (pe == 1.0) ? 0.0 : (po - pe) / (1.0 - pe);
        break;
    }
    }
    return JS_NewFloat64(ctx, r);
}

JSValue dyn_metric_fbeta(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double *yt = NULL, *yp = NULL, b = 1.0, pos = 1.0, p, q, b2, r;
    size_t n;
    dyn_binconf c;

    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "ml.fbeta: expected (yTrue, yPred, beta[, positive])");
    if (JS_ToFloat64(ctx, &b, argv[2]))
        return JS_EXCEPTION;
    if (argc >= 4 && !JS_IsUndefined(argv[3]) && JS_ToFloat64(ctx, &pos, argv[3]))
        return JS_EXCEPTION;
    if (!(b > 0.0))
        return JS_ThrowRangeError(ctx, "ml.fbeta: beta must be positive");
    if (dyn_metric_pair(ctx, argv[0], argv[1], &yt, &yp, &n))
        return JS_EXCEPTION;
    dyn_fiber_offload();
    c = dyn_binconf_of(yt, yp, n, pos);
    free(yt);
    free(yp);
    p = (c.tp + c.fp) == 0.0 ? 0.0 : c.tp / (c.tp + c.fp);
    q = (c.tp + c.fn_) == 0.0 ? 0.0 : c.tp / (c.tp + c.fn_);
    b2 = b * b;
    r = (b2 * p + q) == 0.0 ? 0.0 : (1.0 + b2) * p * q / (b2 * p + q);
    return JS_NewFloat64(ctx, r);
}

static int dyn_auc_cmp(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return (x < y) ? -1 : (x > y) ? 1
                                  : 0;
}

static int dyn_ap_cmp(const void* a, const void* b)
{
    const dyn_ap_kv_t *x = (const dyn_ap_kv_t*)a, *y = (const dyn_ap_kv_t*)b;
    if (x->s != y->s)
        return (x->s < y->s) ? 1 : -1;
    return (x->i > y->i) ? 1 : (x->i < y->i) ? -1
                                             : 0;
}

JSValue dyn_metric_auc(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int want_pr)
{
    double *yt = NULL, *ys = NULL, pos = 1.0, r = 0.0;
    size_t n, i;
    double npos = 0.0, nneg = 0.0;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:ml expected (yTrue, yScore[, positive])");
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && JS_ToFloat64(ctx, &pos, argv[2]))
        return JS_EXCEPTION;
    if (dyn_metric_pair(ctx, argv[0], argv[1], &yt, &ys, &n))
        return JS_EXCEPTION;
    dyn_fiber_offload();
    for (i = 0; i < n; i++) {
        if (!isfinite(yt[i]) || !isfinite(ys[i])) {
            free(yt);
            free(ys);
            return JS_ThrowRangeError(ctx,
                "dyna:ml rocAuc scores and labels must be finite");
        }
    }
    for (i = 0; i < n; i++) {
        if (yt[i] == pos)
            npos += 1.0;
        else
            nneg += 1.0;
    }
    if (npos == 0.0 || nneg == 0.0) {
        free(yt);
        free(ys);
        return JS_ThrowRangeError(ctx,
            "dyna:ml rocAuc: needs both a positive and a negative sample");
    }

    if (!want_pr) {
        double* sorted = (double*)malloc(n * sizeof(double));
        double ranksum = 0.0;
        if (!sorted) {
            free(yt);
            free(ys);
            return JS_ThrowOutOfMemory(ctx);
        }
        memcpy(sorted, ys, n * sizeof(double));
        dyn_ml_isort(sorted, n, sizeof(double), dyn_auc_cmp);
        for (i = 0; i < n; i++) {
            if (yt[i] != pos)
                continue;
            {
                size_t lo = 0, hi = n, mid, first, last;
                while (lo < hi) {
                    mid = (lo + hi) / 2;
                    if (sorted[mid] < ys[i])
                        lo = mid + 1;
                    else
                        hi = mid;
                }
                first = lo;
                lo = 0;
                hi = n;
                while (lo < hi) {
                    mid = (lo + hi) / 2;
                    if (sorted[mid] <= ys[i])
                        lo = mid + 1;
                    else
                        hi = mid;
                }
                last = lo;
                ranksum += ((double)(first + 1) + (double)last) / 2.0;
            }
        }
        free(sorted);
        r = (ranksum - npos * (npos + 1.0) / 2.0) / (npos * nneg);
    } else {
        dyn_ap_kv_t* kv = (dyn_ap_kv_t*)malloc(n * sizeof(*kv));
        double tp = 0.0, fp = 0.0, ap = 0.0;
        size_t at = 0;
        if (!kv) {
            free(yt);
            free(ys);
            return JS_ThrowOutOfMemory(ctx);
        }
        for (i = 0; i < n; i++) {
            kv[i].s = ys[i];
            kv[i].i = i;
        }
        dyn_ml_isort(kv, n, sizeof(*kv), dyn_ap_cmp);
        while (at < n) {
            size_t j = at;
            double tp_end = tp, fp_end = fp;
            do {
                if (yt[kv[j].i] == pos)
                    tp_end += 1.0;
                else
                    fp_end += 1.0;
                j++;
            } while (j < n && kv[j].s == kv[at].s);
            ap += (tp_end - tp) * tp_end / (tp_end + fp_end);
            tp = tp_end;
            fp = fp_end;
            at = j;
        }
        free(kv);
        r = ap / npos;
    }
    free(yt);
    free(ys);
    return JS_NewFloat64(ctx, r);
}

JSValue dyn_metric_confusion(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double *yt = NULL, *yp = NULL;
    size_t n, i, nc;
    double maxlab = 0.0;
    double* cm;
    JSValue out;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ml.confusionMatrix: expected (yTrue, yPred)");
    if (dyn_metric_pair(ctx, argv[0], argv[1], &yt, &yp, &n))
        return JS_EXCEPTION;
    dyn_fiber_offload();
    for (i = 0; i < n; i++) {
        double a = yt[i], b = yp[i];
        if (!(a >= 0.0) || !(b >= 0.0) || a != floor(a) || b != floor(b)) {
            free(yt);
            free(yp);
            return JS_ThrowRangeError(ctx,
                "ml.confusionMatrix: confusionMatrix labels must be non-negative integers");
        }
        if (a > maxlab)
            maxlab = a;
        if (b > maxlab)
            maxlab = b;
    }
    if (maxlab > 4095.0) {
        free(yt);
        free(yp);
        return JS_ThrowRangeError(ctx,
            "ml.confusionMatrix: confusionMatrix supports labels up to 4095");
    }
    nc = (size_t)maxlab + 1;
    cm = (double*)calloc(nc * nc, sizeof(double));
    if (!cm) {
        free(yt);
        free(yp);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < n; i++)
        cm[(size_t)yt[i] * nc + (size_t)yp[i]] += 1.0;
    free(yt);
    free(yp);
    out = dyn_ml_matrix_to_js(ctx, cm, nc, nc, 0);
    free(cm);
    return out;
}

int dyn_ml_score_fold(JSContext* ctx, JSValueConst factory,
    JSValueConst scorer, const dyn_matrix_t* mx,
    const double* y, const uint32_t* tr, size_t ntr,
    const uint32_t* te, size_t nte, double* out)
{
    JSValue model = JS_UNDEFINED, Xtr = JS_UNDEFINED, ytr = JS_UNDEFINED;
    JSValue Xte = JS_UNDEFINED, yte = JS_UNDEFINED, pred = JS_UNDEFINED;
    JSValue fit = JS_UNDEFINED, predict = JS_UNDEFINED, r = JS_UNDEFINED;
    JSValueConst args[2];
    int rc = -1;

    model = JS_Call(ctx, factory, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(model))
        goto done;
    if (!JS_IsObject(model)) {
        JS_ThrowTypeError(ctx, "dyna:ml estimator must be a factory returning a model, "
                               "e.g. () => new LogisticRegression()");
        goto done;
    }
    Xtr = dyn_ml_rows_subset(ctx, mx, tr, ntr);
    ytr = dyn_ml_vec_subset(ctx, y, tr, ntr);
    Xte = dyn_ml_rows_subset(ctx, mx, te, nte);
    yte = dyn_ml_vec_subset(ctx, y, te, nte);
    if (JS_IsException(Xtr) || JS_IsException(ytr) || JS_IsException(Xte) || JS_IsException(yte))
        goto done;

    fit = JS_GetPropertyStr(ctx, model, "fit");
    predict = JS_GetPropertyStr(ctx, model, "predict");
    if (JS_IsException(fit) || JS_IsException(predict) || !JS_IsFunction(ctx, fit) || !JS_IsFunction(ctx, predict)) {
        JS_ThrowTypeError(ctx, "dyna:ml the estimator needs fit() and predict()");
        goto done;
    }
    args[0] = Xtr;
    args[1] = ytr;
    r = JS_Call(ctx, fit, model, 2, args);
    if (JS_IsException(r))
        goto done;
    JS_FreeValue(ctx, r);
    r = JS_UNDEFINED;
    pred = JS_Call(ctx, predict, model, 1, (JSValueConst*)&Xte);
    if (JS_IsException(pred))
        goto done;
    if (dyn_ml_apply_scorer(ctx, scorer, yte, pred, out))
        goto done;
    rc = 0;
done:
    if (JS_IsObject(model) && !rc) {
        JSValue close = JS_GetPropertyStr(ctx, model, "close");
        if (JS_IsException(close)) {
            rc = -1;
        } else {
            if (JS_IsFunction(ctx, close)) {
                JSValue cr = JS_Call(ctx, close, model, 0, NULL);
                if (JS_IsException(cr))
                    rc = -1;
                JS_FreeValue(ctx, cr);
            }
            JS_FreeValue(ctx, close);
        }
    }
    JS_FreeValue(ctx, model);
    JS_FreeValue(ctx, Xtr);
    JS_FreeValue(ctx, ytr);
    JS_FreeValue(ctx, Xte);
    JS_FreeValue(ctx, yte);
    JS_FreeValue(ctx, pred);
    JS_FreeValue(ctx, fit);
    JS_FreeValue(ctx, predict);
    JS_FreeValue(ctx, r);
    return rc;
}

#endif
