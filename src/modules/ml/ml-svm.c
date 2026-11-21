#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

typedef struct {
    const dyn_svm_t* m;
    const double* X;
    const double* yb;
    size_t rows, cols;
    double gamma;
    double* alpha;
    double* f;
    double* krow_i;
    double* krow_j;
    double** kcache;
    size_t kbudget;
    double b;
} dyn_svm_smo_t;

#define DYN_SVM_KCACHE_BYTES ((size_t)64 << 20)

static const char* const ml_svm_keys[] = { "kernel", "C", "gamma", "coef0",
    "tol", "degree", "maxIter" };

JSValue dyn_svm_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_svm_t* m;
    dyn_svm_kernel_t kern = DYN_SVM_RBF;
    double C = 1.0, gamma = 0.0, coef0 = 0.0, tol = 1e-3;
    size_t max_iter = 1000, degree_holder = 3;

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst o = argv[0];
        if (dyn_opts_strict(ctx, o, ml_svm_keys, 7))
            return JS_EXCEPTION;
        JSValue kv = JS_GetPropertyStr(ctx, o, "kernel");
        if (JS_IsException(kv))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(kv)) {
            const char* k = JS_ToCString(ctx, kv);
            JS_FreeValue(ctx, kv);
            if (!k)
                return JS_EXCEPTION;
            if (!strcmp(k, "linear"))
                kern = DYN_SVM_LINEAR;
            else if (!strcmp(k, "rbf"))
                kern = DYN_SVM_RBF;
            else if (!strcmp(k, "poly"))
                kern = DYN_SVM_POLY;
            else {
                JS_FreeCString(ctx, k);
                return JS_ThrowTypeError(ctx,
                    "dyna:ml kernel must be \"linear\", \"rbf\" or \"poly\"");
            }
            JS_FreeCString(ctx, k);
        } else {
            JS_FreeValue(ctx, kv);
        }
        if (dyn_opt_double(ctx, o, "C", &C, 1e-12, 1e12) || dyn_opt_double(ctx, o, "gamma", &gamma, 0.0, 1e12) || dyn_opt_double(ctx, o, "coef0", &coef0, -1e12, 1e12) || dyn_opt_double(ctx, o, "tol", &tol, 1e-15, 1.0) || dyn_opt_size(ctx, o, "degree", &degree_holder, 1) || dyn_opt_size(ctx, o, "maxIter", &max_iter, 1))
            return JS_EXCEPTION;
    } else if (argc > 0 && !JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "dyna:ml expected an options object");
    }
    if (degree_holder > 1000)
        return JS_ThrowRangeError(ctx, "dyna:ml degree must be at most 1000");
    if (max_iter > DYN_ML_MAX_ITERS)
        return JS_ThrowRangeError(ctx, "dyna:ml maxIter must be at most %u",
            (unsigned)DYN_ML_MAX_ITERS);
    m = (dyn_svm_t*)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->kernel = kern;
    m->C = C;
    m->gamma = gamma;
    m->coef0 = coef0;
    m->degree = (int)degree_holder;
    m->tol = tol;
    m->max_iter = max_iter;
    return dyn_res_wrap(ctx, new_target, dyn_svm_class_id, m, dyn_svm_dispose);
}

static double dyn_svm_k(const dyn_svm_t* m, const double* a, const double* b,
    size_t d, double gamma)
{
    switch ((int)(m->kernel)) {
    case DYN_SVM_LINEAR:
        return dyn_ml_dot(a, b, d);
    case DYN_SVM_POLY: {
        double base = gamma * dyn_ml_dot(a, b, d) + m->coef0;
        double r = 1.0;
        int e;
        for (e = 0; e < m->degree; e++)
            r *= base;
        return r;
    }
    default:
        return exp(-gamma * dyn_ml_sqdist(a, b, d));
    }
}

static const double* dyn_svm_krow(JSContext* ctx, dyn_svm_smo_t* st, size_t i,
    double* buf)
{
    const double* X = st->X;
    size_t rows = st->rows, cols = st->cols, s;
    double* row;
    if (st->kcache && st->kcache[i])
        return st->kcache[i];
    row = buf;
    if (st->kcache && st->kbudget >= rows) {
        double* r = (double*)malloc(rows * sizeof(double));
        if (r) {
            row = r;
            st->kcache[i] = r;
            st->kbudget -= rows;
        }
    }
    for (s = 0; s < rows; s++) {
        row[s] = dyn_svm_k(st->m, X + i * cols, X + s * cols, cols, st->gamma);
        if (!isfinite(row[s])) {
            JS_ThrowRangeError(ctx,
                "ml.fit: polynomial kernel overflowed; scale X or lower degree");
            return NULL;
        }
    }
    return row;
}

static int dyn_svm_pair_step(JSContext* ctx, dyn_svm_smo_t* st, size_t i, size_t j)
{
    const dyn_svm_t* m = st->m;
    const double* X = st->X;
    const double* yb = st->yb;
    size_t rows = st->rows, cols = st->cols, s;
    double gamma = st->gamma, b = st->b;
    double Ei = st->f[i] - yb[i], Ej = st->f[j] - yb[j];
    double ai_old = st->alpha[i], aj_old = st->alpha[j];
    double L, H, eta, kii, kjj, kij, aj_new, ai_new, b1, b2, b_new, di, dj;

    if (yb[i] != yb[j]) {
        L = fmax(0.0, aj_old - ai_old);
        H = fmin(m->C, m->C + aj_old - ai_old);
    } else {
        L = fmax(0.0, ai_old + aj_old - m->C);
        H = fmin(m->C, ai_old + aj_old);
    }
    if (H - L < 1e-15)
        return 0;
    kii = dyn_svm_k(m, X + i * cols, X + i * cols, cols, gamma);
    kjj = dyn_svm_k(m, X + j * cols, X + j * cols, cols, gamma);
    kij = dyn_svm_k(m, X + i * cols, X + j * cols, cols, gamma);
    if (!isfinite(kii) || !isfinite(kjj) || !isfinite(kij)) {
        JS_ThrowRangeError(ctx,
            "ml.fit: polynomial kernel overflowed; scale X or lower degree");
        return -1;
    }
    eta = kii + kjj - 2.0 * kij;
    if (!isfinite(eta) || eta <= 1e-15)
        return 0;
    aj_new = aj_old + yb[j] * (Ei - Ej) / eta;
    if (aj_new > H)
        aj_new = H;
    if (aj_new < L)
        aj_new = L;
    if (fabs(aj_new - aj_old) < 1e-12)
        return 0;
    ai_new = ai_old + yb[i] * yb[j] * (aj_old - aj_new);
    di = yb[i] * (ai_new - ai_old);
    dj = yb[j] * (aj_new - aj_old);
    b1 = b - Ei - di * kii - dj * kij;
    b2 = b - Ej - di * kij - dj * kjj;
    if (ai_new > 0.0 && ai_new < m->C)
        b_new = b1;
    else if (aj_new > 0.0 && aj_new < m->C)
        b_new = b2;
    else
        b_new = 0.5 * (b1 + b2);
    {
        const double *ki = dyn_svm_krow(ctx, st, i, st->krow_i), *kj;
        if (!ki)
            return -1;
        kj = dyn_svm_krow(ctx, st, j, st->krow_j);
        if (!kj)
            return -1;
        for (s = 0; s < rows; s++)
            st->f[s] += di * ki[s] + dj * kj[s] + (b_new - b);
    }
    st->alpha[i] = ai_new;
    st->alpha[j] = aj_new;
    st->b = b_new;
    return 1;
}

static int dyn_svm_train_bin(JSContext* ctx, dyn_svm_t* m, const double* X,
    const double* yb, size_t rows, size_t cols,
    double gamma, dyn_svm_bin_t* out, uint64_t* rng)
{
    double *alpha = NULL, *f = NULL, *krow_i = NULL, *krow_j = NULL;
    double b = 0.0;
    size_t iter, i, j, nsv;
    dyn_svm_smo_t st = { 0 };
    int rc = -1;

    alpha = (double*)calloc(rows, sizeof(double));
    f = (double*)calloc(rows, sizeof(double));
    krow_i = (double*)malloc(rows * sizeof(double));
    krow_j = (double*)malloc(rows * sizeof(double));
    if (!alpha || !f || !krow_i || !krow_j) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    st.m = m;
    st.X = X;
    st.yb = yb;
    st.rows = rows;
    st.cols = cols;
    st.gamma = gamma;
    st.alpha = alpha;
    st.f = f;
    st.krow_i = krow_i;
    st.krow_j = krow_j;
    st.kbudget = DYN_SVM_KCACHE_BYTES / sizeof(double);
    st.kcache = (double**)calloc(rows, sizeof(double*));
    st.b = 0.0;
    for (iter = 0; iter < m->max_iter; iter++) {
        size_t changed = 0;
        for (i = 0; i < rows; i++) {
            double ri = yb[i] * (f[i] - yb[i]);
            size_t t;
            int moved = 0;
            if (JS_CheckInterrupt(ctx))
                goto done;
            if (!((ri < -m->tol && alpha[i] < m->C) || (ri > m->tol && alpha[i] > 0.0)))
                continue;
            j = (size_t)(dyn_splitmix64(rng) % (uint64_t)rows);
            for (t = 0; t < rows && !moved; t++) {
                size_t jj = j + t < rows ? j + t : j + t - rows;
                if (jj == i)
                    continue;
                moved = dyn_svm_pair_step(ctx, &st, i, jj);
                if (moved < 0)
                    goto done;
            }
            if (moved)
                changed++;
        }
        if (changed == 0)
            break;
    }
    b = st.b;
    nsv = 0;
    for (i = 0; i < rows; i++)
        if (alpha[i] > 1e-12)
            nsv++;
    out->alpha = (double*)malloc((nsv ? nsv : 1) * sizeof(double));
    out->sv = (double*)malloc((nsv ? nsv * cols : 1) * sizeof(double));
    if (!out->alpha || !out->sv) {
        free(out->alpha);
        free(out->sv);
        out->alpha = NULL;
        out->sv = NULL;
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    nsv = 0;
    for (i = 0; i < rows; i++) {
        if (!(alpha[i] > 1e-12))
            continue;
        out->alpha[nsv] = alpha[i] * yb[i];
        memcpy(out->sv + nsv * cols, X + i * cols, cols * sizeof(double));
        nsv++;
    }
    out->n_sv = nsv;
    out->b = b;
    rc = 0;
done:
    if (st.kcache) {
        for (i = 0; i < rows; i++)
            free(st.kcache[i]);
        free(st.kcache);
    }
    free(alpha);
    free(f);
    free(krow_i);
    free(krow_j);
    return rc;
}

static double dyn_svm_decide(const dyn_svm_t* m, const dyn_svm_bin_t* bin,
    const double* x, double gamma)
{
    double s = bin->b;
    size_t i;
    for (i = 0; i < bin->n_sv; i++)
        s += bin->alpha[i] * dyn_svm_k(m, bin->sv + i * m->cols, x, m->cols, gamma);
    return s;
}

static int dyn_svm_learn(JSContext* ctx, dyn_svm_t* m, const double* X,
    const double* y, size_t rows, size_t cols)
{
    double *classes = NULL, *yb = NULL;
    dyn_svm_bin_t* bin = NULL;
    size_t nc, nb, k, i;
    double gamma;
    uint64_t rng = 987654321ULL;
    int rc = -1;

    nc = dyn_ml_classes(ctx, y, rows, &classes);
    if (nc == 0)
        return -1;
    if (nc < 2) {
        free(classes);
        JS_ThrowRangeError(ctx, "ml.fit: SVC needs at least two classes");
        return -1;
    }
    if (nc > 256) {
        free(classes);
        JS_ThrowRangeError(ctx, "ml.fit: SVC supports at most 256 classes, found %u",
            (unsigned)nc);
        return -1;
    }
    gamma = (m->gamma > 0.0) ? m->gamma : 1.0 / (double)cols;
    nb = (nc == 2) ? 1 : nc;
    bin = (dyn_svm_bin_t*)calloc(nb, sizeof(*bin));
    yb = (double*)malloc(rows * sizeof(double));
    if (!bin || !yb) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    m->cols = cols;
    for (k = 0; k < nb; k++) {
        double pos = (nc == 2) ? classes[1] : classes[k];
        for (i = 0; i < rows; i++)
            yb[i] = (y[i] == pos) ? 1.0 : -1.0;
        if (dyn_svm_train_bin(ctx, m, X, yb, rows, cols, gamma, &bin[k], &rng))
            goto done;
    }
    if (m->bin) {
        for (k = 0; k < m->n_bin; k++) {
            free(m->bin[k].alpha);
            free(m->bin[k].sv);
        }
        free(m->bin);
    }
    free(m->classes);
    m->bin = bin;
    m->n_bin = nb;
    m->classes = classes;
    m->n_classes = nc;
    m->gamma = gamma;
    m->fitted = 1;
    bin = NULL;
    classes = NULL;
    rc = 0;
done:
    if (bin) {
        for (k = 0; k < nb; k++) {
            free(bin[k].alpha);
            free(bin[k].sv);
        }
        free(bin);
    }
    free(classes);
    free(yb);
    return rc;
}

JSValue dyn_svm_fit(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    if (dyn_ml_reject_weights(ctx, argc, argv, 2, "SVC"))
        return JS_EXCEPTION;
    dyn_svm_t* m;
    dyn_matrix_t mx = { 0 };
    double* y = NULL;
    JSValueConst rows_arg, cols_arg;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ml.fit: fit(X, y) requires two arguments");
    rows_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    cols_arg = argc > 3 ? argv[3] : JS_UNDEFINED;
    if (dyn_ml_ingest_Xy_no_w(ctx, argv[0], argv[1], rows_arg, cols_arg,
            &mx, &y))
        return JS_EXCEPTION;
    m = (dyn_svm_t*)dyn_res_native(ctx, this_val, dyn_svm_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        free(y);
        return JS_EXCEPTION;
    }
    dyn_fiber_offload();
    if (dyn_svm_learn(ctx, m, mx.data, y, mx.rows, mx.cols)) {
        dyn_matrix_free(&mx);
        free(y);
        return JS_EXCEPTION;
    }
    dyn_matrix_free(&mx);
    free(y);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_svm_predict_impl(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int decision,
    int into)
{
    dyn_svm_t* m;
    dyn_matrix_t mx = { 0 };
    dyn_csr_t* S = NULL;
    dyn_ml_row_src rs = { 0 };
    double* out = NULL;
    size_t rows, i, k, ocols;
    int flat, as_f64 = 0;
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
    if (S ? dyn_ml_check_csr_values_finite(ctx, S)
          : dyn_ml_check_finite_fast(ctx, &mx)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    m = (dyn_svm_t*)dyn_res_native(ctx, this_val, dyn_svm_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return dyn_ml_throw_unfitted(ctx, this_val, "predict");
    }
    if ((S ? S->cols : mx.cols) != m->cols) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "ml.predict: X has %llu features, model expects %llu",
            (unsigned long long)(S ? S->cols : mx.cols),
            (unsigned long long)m->cols);
    }
    rows = S ? S->rows : mx.rows;
    flat = !S && mx.flat;
    ocols = decision ? m->n_bin : 1;
    if (rows > (SIZE_MAX / sizeof(double)) / ocols) {
        dyn_matrix_free(&mx);
        return JS_ThrowRangeError(ctx, "ml.predict: data too large for SVC predict");
    }
    if (into) {
        if (decision) {
            dyn_matrix_free(&mx);
            return JS_ThrowTypeError(ctx,
                "ml.decisionInto: not provided; read decisionFunction's output "
                "columns from predict/predictInto instead");
        }
        if (dyn_ml_bind_out(ctx, argv[0], rows, &out)) {
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
    } else {
        out = (double*)malloc(rows * ocols * sizeof(double));
        if (!out) {
            dyn_matrix_free(&mx);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    dyn_fiber_offload();
    if (dyn_ml_row_src_init(ctx, &rs, &mx, S)) {
        if (!into)
            free(out);
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    for (i = 0; i < rows; i++) {
        const double* xi = dyn_ml_row_at(&rs, i);
#define DYN_SVM_REFUSE_NONFINITE(dv)                                                  \
    do {                                                                              \
        if (!isfinite(dv)) {                                                          \
            if (!into)                                                                \
                free(out);                                                            \
            dyn_ml_row_src_free(&rs);                                                 \
            dyn_matrix_free(&mx);                                                     \
            return JS_ThrowRangeError(ctx,                                            \
                "ml.predict: polynomial kernel overflowed; scale X or lower degree"); \
        }                                                                             \
    } while (0)
        if (decision) {
            for (k = 0; k < m->n_bin; k++) {
                out[i * ocols + k] = dyn_svm_decide(m, &m->bin[k], xi, m->gamma);
                DYN_SVM_REFUSE_NONFINITE(out[i * ocols + k]);
            }
        } else if (m->n_bin == 1) {
            double d0 = dyn_svm_decide(m, &m->bin[0], xi, m->gamma);
            DYN_SVM_REFUSE_NONFINITE(d0);
            out[i] = (d0 >= 0.0) ? m->classes[1] : m->classes[0];
        } else {
            double best = -HUGE_VAL;
            size_t bestk = 0;
            for (k = 0; k < m->n_bin; k++) {
                double d = dyn_svm_decide(m, &m->bin[k], xi, m->gamma);
                DYN_SVM_REFUSE_NONFINITE(d);
                if (d > best) {
                    best = d;
                    bestk = k;
                }
            }
            out[i] = m->classes[bestk];
        }
    }
#undef DYN_SVM_REFUSE_NONFINITE
    dyn_ml_row_src_free(&rs);
    dyn_matrix_free(&mx);
    if (into)
        return JS_NewInt64(ctx, (int64_t)rows);
    result = (decision && ocols > 1)
        ? dyn_ml_matrix_to_js(ctx, out, rows, ocols, flat)
        : dyn_ml_predict_result(ctx, out, rows, as_f64 || (decision && flat));
    free(out);
    return result;
}

JSValue dyn_svm_predict(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_svm_predict_impl(ctx, t, argc, argv, 0, 0);
}

JSValue dyn_svm_decision(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_svm_predict_impl(ctx, t, argc, argv, 1, 0);
}

JSValue dyn_svm_predict_into(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_svm_predict_impl(ctx, t, argc, argv, 0, 1);
}

JSValue dyn_svm_nsv(JSContext* ctx, JSValueConst this_val)
{
    dyn_svm_t* m = (dyn_svm_t*)dyn_res_native(ctx, this_val, dyn_svm_class_id);
    size_t k, tot = 0;
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewInt32(ctx, 0);
    for (k = 0; k < m->n_bin; k++)
        tot += m->bin[k].n_sv;
    return JS_NewInt64(ctx, (int64_t)tot);
}

JSValue dyn_svm_classes_get(JSContext* ctx, JSValueConst this_val)
{
    dyn_svm_t* m = (dyn_svm_t*)dyn_res_native(ctx, this_val, dyn_svm_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_doubles_to_js(ctx, m->classes, m->n_classes);
}

#endif
