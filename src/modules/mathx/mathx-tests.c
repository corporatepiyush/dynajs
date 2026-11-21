#include "mathx-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_MATHX)

#include "core/dyn-statk.h"

#define MXT_MAX_ELEMS (1u << 24)

typedef struct {
    const char* name;
    int nparams;
    double (*f1)(double);
    double (*f2)(double, double);
    double (*f3)(double, double, double);
} mxt_dist_t;

static const mxt_dist_t mxt_dists[] = {
    { "normPdf", 0, dyn_st_norm_pdf, NULL, NULL },
    { "normCdf", 0, dyn_st_norm_cdf, NULL, NULL },
    { "normSf", 0, dyn_st_norm_sf, NULL, NULL },
    { "normPpf", 0, dyn_st_norm_ppf, NULL, NULL },
    { "tPdf", 1, NULL, dyn_st_t_pdf, NULL },
    { "tCdf", 1, NULL, dyn_st_t_cdf, NULL },
    { "tSf", 1, NULL, dyn_st_t_sf, NULL },
    { "tPpf", 1, NULL, dyn_st_t_ppf, NULL },
    { "chi2Pdf", 1, NULL, dyn_st_chi2_pdf, NULL },
    { "chi2Cdf", 1, NULL, dyn_st_chi2_cdf, NULL },
    { "chi2Sf", 1, NULL, dyn_st_chi2_sf, NULL },
    { "chi2Ppf", 1, NULL, dyn_st_chi2_ppf, NULL },
    { "fCdf", 2, NULL, NULL, dyn_st_f_cdf },
    { "fSf", 2, NULL, NULL, dyn_st_f_sf },
    { "fPpf", 2, NULL, NULL, dyn_st_f_ppf },
    { "binomPmf", 2, NULL, NULL, dyn_st_binom_pmf },
    { "binomCdf", 2, NULL, NULL, dyn_st_binom_cdf },
    { "binomSf", 2, NULL, NULL, dyn_st_binom_sf },
    { "poissonPmf", 1, NULL, dyn_st_poisson_pmf, NULL },
    { "poissonCdf", 1, NULL, dyn_st_poisson_cdf, NULL },
    { "poissonSf", 1, NULL, dyn_st_poisson_sf, NULL },
    { "kolmogorovSf", 0, dyn_st_kolmogorov_sf, NULL, NULL },
};

int mxt_vec(JSContext* ctx, JSValueConst v, const char* fn, const char* what, double** out, uint32_t* n)
{
    mxst_src s;
    uint32_t i;
    double* buf;
    *out = NULL;
    *n = 0;
    if (!JS_IsObject(v)) {
        JS_ThrowTypeError(ctx, "%s: %s must be an array of numbers", fn, what);
        return -1;
    }
    if (mxst_src_of(ctx, v, &s))
        return -1;
    if (s.len > MXT_MAX_ELEMS) {
        JS_ThrowRangeError(ctx, "%s: %s longer than 16777216", fn, what);
        return -1;
    }
    buf = (double*)malloc((size_t)(s.len ? s.len : 1) * sizeof(double));
    if (!buf) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (s.p) {
        memcpy(buf, s.p, (size_t)s.len * sizeof(double));
    } else {
        for (i = 0; i < s.len; i++)
            if (mxst_elem(ctx, s.arr, i, &buf[i])) {
                free(buf);
                return -1;
            }
    }
    *out = buf;
    *n = s.len;
    return 0;
}

static int mxt_has_nan(const double* x, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (isnan(x[i]))
            return 1;
    return 0;
}

JSValue mxt_f64(JSContext* ctx, double* p, uint32_t n)
{
    JSValueConst args[3];
    JSValue ab, out;
    if (!p) {
        p = (double*)malloc(1);
        if (!p)
            return JS_ThrowOutOfMemory(ctx);
    }
    ab = dyn_nat_ab_take(ctx, p, (size_t)n * sizeof(double));
    if (JS_IsException(ab))
        return ab;
    args[0] = ab;
    args[1] = JS_UNDEFINED;
    args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_FLOAT64);
    JS_FreeValue(ctx, ab);
    return out;
}

static double mxt_apply(const mxt_dist_t* d, double x, const double* p)
{
    if (d->f1)
        return d->f1(x);
    if (d->f2)
        return d->f2(x, p[0]);
    return d->f3(x, p[0], p[1]);
}

static JSValue mxt_dist(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    const mxt_dist_t* d = &mxt_dists[magic];
    double p[2] = { 0.0, 0.0 }, x, *v;
    uint32_t n;
    int k;
    (void)this_val;
    for (k = 0; k < d->nparams; k++) {
        if (k + 1 >= argc || !JS_IsNumber(argv[k + 1]))
            return JS_ThrowTypeError(ctx, "stats.%s: parameter %d must be a number", d->name, k + 1);
        if (JS_ToFloat64(ctx, &p[k], argv[k + 1]))
            return JS_EXCEPTION;
    }
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "stats.%s: x must be a number or an array of numbers", d->name);
    if (JS_IsNumber(argv[0])) {
        if (JS_ToFloat64(ctx, &x, argv[0]))
            return JS_EXCEPTION;
        return JS_NewFloat64(ctx, mxt_apply(d, x, p));
    }
    if (mxt_vec(ctx, argv[0], d->name, "x", &v, &n))
        return JS_EXCEPTION;
    {
        dyn_st_map_t m = { d->f1, d->f2, d->f3, p[0], p[1] };
        dyn_st_map(&m, v, n);
    }
    return mxt_f64(ctx, v, n);
}

int mxt_opts_check(JSContext* ctx, JSValueConst opts, const char* fn, const char* const* keys, int nkeys)
{
    JSPropertyEnum* tab;
    uint32_t len, i;
    int k, bad = 0;
    if (JS_IsUndefined(opts) || JS_IsNull(opts))
        return 0;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "%s: opts must be an object", fn);
        return -1;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, opts, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < len && !bad; i++) {
        const char* s = JS_AtomToCString(ctx, tab[i].atom);
        int ok = 0;
        if (!s) {
            bad = -1;
            break;
        }
        for (k = 0; k < nkeys; k++)
            if (strcmp(s, keys[k]) == 0)
                ok = 1;
        if (!ok) {
            JS_ThrowTypeError(ctx, "%s: unknown option '%s'", fn, s);
            bad = -1;
        }
        JS_FreeCString(ctx, s);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return bad;
}

int mxt_get(JSContext* ctx, JSValueConst opts, const char* key, JSValue* out)
{
    *out = JS_UNDEFINED;
    if (JS_IsUndefined(opts) || JS_IsNull(opts))
        return 0;
    *out = JS_GetPropertyStr(ctx, opts, key);
    return JS_IsException(*out) ? -1 : 0;
}

int mxt_enum(JSContext* ctx, JSValueConst opts, const char* fn, const char* key, const char* const* names,
    int nn, int def, int* out)
{
    JSValue v;
    const char* s;
    int k;
    *out = def;
    if (mxt_get(ctx, opts, key, &v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "%s: %s must be a string", fn, key);
        return -1;
    }
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    for (k = 0; k < nn; k++)
        if (strcmp(s, names[k]) == 0) {
            *out = k;
            JS_FreeCString(ctx, s);
            return 0;
        }
    JS_ThrowRangeError(ctx, "%s: unknown %s '%s'", fn, key, s);
    JS_FreeCString(ctx, s);
    return -1;
}

int mxt_bool(JSContext* ctx, JSValueConst opts, const char* fn, const char* key, int def, int* out)
{
    JSValue v;
    *out = def;
    if (mxt_get(ctx, opts, key, &v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsBool(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "%s: %s must be a boolean", fn, key);
        return -1;
    }
    *out = JS_ToBool(ctx, v);
    return 0;
}

int mxt_num(JSContext* ctx, JSValueConst opts, const char* fn, const char* key, double def, double* out)
{
    JSValue v;
    *out = def;
    if (mxt_get(ctx, opts, key, &v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsNumber(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "%s: %s must be a number", fn, key);
        return -1;
    }
    return JS_ToFloat64(ctx, out, v) ? -1 : 0;
}

static const char* const mxt_alt_names[] = { "two-sided", "less", "greater" };
static const char* const mxt_method_names[] = { "auto", "exact", "asymptotic" };

static int mxt_alt(JSContext* ctx, JSValueConst opts, const char* fn, dyn_st_alt_t* alt)
{
    int v;
    if (mxt_enum(ctx, opts, fn, "alternative", mxt_alt_names, 3, 0, &v))
        return -1;
    *alt = (dyn_st_alt_t)v;
    return 0;
}

static JSValue mxt_throw(JSContext* ctx, const char* fn, int rc)
{
    switch (rc) {
    case DYN_ST_E_OOM:
        return JS_ThrowOutOfMemory(ctx);
    case DYN_ST_E_NAN:
        return JS_ThrowRangeError(ctx, "%s: input contains NaN", fn);
    case DYN_ST_E_SIZE:
        return JS_ThrowRangeError(ctx, "%s: sample size out of range", fn);
    case DYN_ST_E_SHAPE:
        return JS_ThrowRangeError(ctx, "%s: table must have at least one row and one column", fn);
    case DYN_ST_E_CANCELLED:
        return JS_ThrowInternalError(ctx, "%s: interrupted", fn);
    case DYN_ST_E_DOMAIN:
    default:
        return JS_ThrowRangeError(ctx, "%s: argument out of domain", fn);
    }
}

static JSValue mxt_result(JSContext* ctx, const dyn_st_result_t* r, int with_df)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (JS_SetPropertyStr(ctx, o, "statistic", JS_NewFloat64(ctx, r->statistic)) < 0
        || JS_SetPropertyStr(ctx, o, "pvalue", JS_NewFloat64(ctx, r->pvalue)) < 0
        || (with_df && JS_SetPropertyStr(ctx, o, "df", JS_NewFloat64(ctx, r->df)) < 0)) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

enum {
    MXT_T1,
    MXT_TIND,
    MXT_TREL,
    MXT_MWU,
    MXT_KS2,
    MXT_PEARSON,
    MXT_SPEARMAN
};

static const char* const mxt_two_names[] = { "stats.ttest1samp", "stats.ttestInd", "stats.ttestRel",
    "stats.mannWhitneyU", "stats.ks2Samp", "stats.pearson", "stats.spearman" };

static JSValue mxt_two(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const k_alt[] = { "alternative" };
    static const char* const k_ind[] = { "alternative", "equalVar" };
    static const char* const k_mwu[] = { "alternative", "method", "continuity" };
    static const char* const k_ks[] = { "alternative", "method" };
    const char* fn = mxt_two_names[magic];
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    double *a = NULL, *b = NULL, mu = 0.0;
    uint32_t na = 0, nb = 0;
    dyn_st_alt_t alt;
    dyn_st_result_t r;
    int rc = DYN_ST_OK, ev = 1, method = 0, cont = 1, with_df = 0;
    (void)this_val;
    if (magic == MXT_TIND      ? mxt_opts_check(ctx, opts, fn, k_ind, 2)
            : magic == MXT_MWU ? mxt_opts_check(ctx, opts, fn, k_mwu, 3)
            : magic == MXT_KS2 ? mxt_opts_check(ctx, opts, fn, k_ks, 2)
                               : mxt_opts_check(ctx, opts, fn, k_alt, 1))
        return JS_EXCEPTION;
    if (mxt_alt(ctx, opts, fn, &alt))
        return JS_EXCEPTION;
    if (magic == MXT_TIND && mxt_bool(ctx, opts, fn, "equalVar", 1, &ev))
        return JS_EXCEPTION;
    if ((magic == MXT_MWU || magic == MXT_KS2) && mxt_enum(ctx, opts, fn, "method", mxt_method_names, 3, 0, &method))
        return JS_EXCEPTION;
    if (magic == MXT_MWU && mxt_bool(ctx, opts, fn, "continuity", 1, &cont))
        return JS_EXCEPTION;
    if (mxt_vec(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, fn, "a", &a, &na))
        return JS_EXCEPTION;
    if (magic == MXT_T1) {
        if (argc < 2 || !JS_IsNumber(argv[1])) {
            free(a);
            return JS_ThrowTypeError(ctx, "%s: popmean must be a number", fn);
        }
        if (JS_ToFloat64(ctx, &mu, argv[1])) {
            free(a);
            return JS_EXCEPTION;
        }
    } else if (mxt_vec(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, fn, "b", &b, &nb)) {
        free(a);
        return JS_EXCEPTION;
    }
    if ((magic == MXT_TREL || magic == MXT_PEARSON || magic == MXT_SPEARMAN) && na != nb) {
        free(a);
        free(b);
        return JS_ThrowRangeError(ctx, "%s: length mismatch (%u vs %u)", fn, na, nb);
    }
    switch (magic) {
    case MXT_T1:
        rc = dyn_st_ttest_1samp(a, na, mu, alt, &r);
        with_df = 1;
        break;
    case MXT_TIND:
        rc = dyn_st_ttest_ind(a, na, b, nb, ev, alt, &r);
        with_df = 1;
        break;
    case MXT_TREL:
        rc = dyn_st_ttest_rel(a, b, na, alt, &r);
        with_df = 1;
        break;
    case MXT_MWU:
        rc = dyn_st_mannwhitney(a, na, b, nb, alt, (dyn_st_method_t)method, cont, &r);
        break;
    case MXT_KS2:
        rc = dyn_st_ks2(a, na, b, nb, alt, (dyn_st_method_t)method, &r);
        break;
    case MXT_PEARSON:
        rc = dyn_st_pearson(a, b, na, alt, &r);
        break;
    case MXT_SPEARMAN:
    default:
        rc = dyn_st_spearman(a, b, na, alt, &r);
        break;
    }
    free(a);
    free(b);
    if (rc != DYN_ST_OK)
        return mxt_throw(ctx, fn, rc);
    return mxt_result(ctx, &r, with_df);
}

static JSValue mxt_ztest(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "alternative" };
    const char* fn = "stats.proportionsZTest";
    double v[4];
    dyn_st_alt_t alt;
    dyn_st_result_t r;
    int k, rc;
    (void)this_val;
    for (k = 0; k < 4; k++) {
        if (k >= argc || !JS_IsNumber(argv[k]))
            return JS_ThrowTypeError(ctx, "%s: counts and totals must be numbers", fn);
        if (JS_ToFloat64(ctx, &v[k], argv[k]))
            return JS_EXCEPTION;
    }
    if (mxt_opts_check(ctx, argc > 4 ? argv[4] : JS_UNDEFINED, fn, keys, 1)
        || mxt_alt(ctx, argc > 4 ? argv[4] : JS_UNDEFINED, fn, &alt))
        return JS_EXCEPTION;
    rc = dyn_st_ztest_prop(v[0], v[1], v[2], v[3], alt, &r);
    if (rc != DYN_ST_OK)
        return mxt_throw(ctx, fn, rc);
    return mxt_result(ctx, &r, 0);
}

static JSValue mxt_chisquare(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "ddof" };
    const char* fn = "stats.chiSquare";
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    double *o = NULL, *e = NULL, ddof;
    uint32_t no, ne = 0;
    dyn_st_result_t r;
    int rc;
    (void)this_val;
    if (mxt_opts_check(ctx, opts, fn, keys, 1) || mxt_num(ctx, opts, fn, "ddof", 0.0, &ddof))
        return JS_EXCEPTION;
    if (!(ddof >= 0.0 && ddof <= 4294967295.0) || ddof != floor(ddof))
        return JS_ThrowRangeError(ctx, "%s: ddof must be a non-negative integer", fn);
    if (mxt_vec(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, fn, "observed", &o, &no))
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        if (mxt_vec(ctx, argv[1], fn, "expected", &e, &ne)) {
            free(o);
            return JS_EXCEPTION;
        }
        if (ne != no) {
            free(o);
            free(e);
            return JS_ThrowRangeError(ctx, "%s: length mismatch (%u vs %u)", fn, no, ne);
        }
    }
    rc = dyn_st_chisquare(o, e, no, (uint32_t)ddof, &r);
    free(o);
    free(e);
    if (rc != DYN_ST_OK)
        return mxt_throw(ctx, fn, rc);
    return mxt_result(ctx, &r, 1);
}

static JSValue mxt_contingency(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "correction" };
    const char* fn = "stats.chi2Contingency";
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue lv, row, res, exp_arr;
    uint32_t rows, cols = 0, i, j, rl;
    double *t = NULL, *ex = NULL, *rv;
    dyn_st_result_t r;
    int corr, rc;
    (void)this_val;
    if (mxt_opts_check(ctx, opts, fn, keys, 1) || mxt_bool(ctx, opts, fn, "correction", 1, &corr))
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "%s: table must be an array of rows", fn);
    lv = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_IsException(lv) || JS_ToUint32(ctx, &rows, lv)) {
        JS_FreeValue(ctx, lv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lv);
    if (rows == 0 || rows > 65536)
        return JS_ThrowRangeError(ctx, "%s: table needs 1..65536 rows", fn);
    for (i = 0; i < rows; i++) {
        row = JS_GetPropertyUint32(ctx, argv[0], i);
        if (JS_IsException(row)) {
            free(t);
            return JS_EXCEPTION;
        }
        rc = mxt_vec(ctx, row, fn, "row", &rv, &rl);
        JS_FreeValue(ctx, row);
        if (rc) {
            free(t);
            return JS_EXCEPTION;
        }
        if (i == 0) {
            cols = rl;
            if (cols == 0 || (uint64_t)rows * cols > MXT_MAX_ELEMS) {
                free(rv);
                return JS_ThrowRangeError(ctx, "%s: table must have 1..16777216 cells", fn);
            }
            t = (double*)malloc((size_t)rows * cols * sizeof(double));
            if (!t) {
                free(rv);
                return JS_ThrowOutOfMemory(ctx);
            }
        } else if (rl != cols) {
            free(rv);
            free(t);
            return JS_ThrowRangeError(ctx, "%s: row %u has %u cells, expected %u", fn, i, rl, cols);
        }
        memcpy(t + (size_t)i * cols, rv, (size_t)cols * sizeof(double));
        free(rv);
    }
    ex = (double*)malloc((size_t)rows * cols * sizeof(double));
    if (!ex) {
        free(t);
        return JS_ThrowOutOfMemory(ctx);
    }
    rc = dyn_st_chi2_contingency(t, rows, cols, corr, ex, &r);
    free(t);
    if (rc != DYN_ST_OK) {
        free(ex);
        return mxt_throw(ctx, fn, rc);
    }
    res = mxt_result(ctx, &r, 1);
    exp_arr = JS_IsException(res) ? JS_EXCEPTION : JS_NewArray(ctx);
    for (i = 0; i < rows && !JS_IsException(exp_arr); i++) {
        double* rowp = (double*)malloc((size_t)cols * sizeof(double));
        JSValue ta;
        if (!rowp) {
            JS_FreeValue(ctx, exp_arr);
            exp_arr = JS_ThrowOutOfMemory(ctx);
            break;
        }
        for (j = 0; j < cols; j++)
            rowp[j] = ex[(size_t)i * cols + j];
        ta = mxt_f64(ctx, rowp, cols);
        if (JS_IsException(ta) || JS_SetPropertyUint32(ctx, exp_arr, i, ta) < 0) {
            JS_FreeValue(ctx, exp_arr);
            exp_arr = JS_EXCEPTION;
        }
    }
    free(ex);
    if (JS_IsException(exp_arr) || JS_SetPropertyStr(ctx, res, "expected", exp_arr) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

static JSValue mxt_rankdata(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "method" };
    static const char* const names[] = { "average", "min", "max", "dense", "ordinal" };
    const char* fn = "stats.rankData";
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    double *x, *out;
    uint32_t n;
    int method, rc;
    (void)this_val;
    if (mxt_opts_check(ctx, opts, fn, keys, 1) || mxt_enum(ctx, opts, fn, "method", names, 5, 0, &method))
        return JS_EXCEPTION;
    if (mxt_vec(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, fn, "a", &x, &n))
        return JS_EXCEPTION;
    if (mxt_has_nan(x, n)) {
        free(x);
        return mxt_throw(ctx, fn, DYN_ST_E_NAN);
    }
    out = (double*)malloc((size_t)(n ? n : 1) * sizeof(double));
    if (!out) {
        free(x);
        return JS_ThrowOutOfMemory(ctx);
    }
    rc = dyn_st_rank(x, n, (dyn_st_rank_t)method, out, NULL);
    free(x);
    if (rc != DYN_ST_OK) {
        free(out);
        return mxt_throw(ctx, fn, rc);
    }
    return mxt_f64(ctx, out, n);
}

static JSValue mxt_padjust(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const names[] = { "bonferroni", "holm", "hochberg", "bh", "by" };
    const char* fn = "stats.pAdjust";
    double *p, *out;
    uint32_t n;
    int method = DYN_ST_ADJ_BH, k, rc;
    const char* s;
    (void)this_val;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (!JS_IsString(argv[1]))
            return JS_ThrowTypeError(ctx, "%s: method must be a string", fn);
        s = JS_ToCString(ctx, argv[1]);
        if (!s)
            return JS_EXCEPTION;
        method = -1;
        for (k = 0; k < 5; k++)
            if (strcmp(s, names[k]) == 0)
                method = k;
        if (method < 0) {
            JS_ThrowRangeError(ctx, "%s: unknown method '%s'", fn, s);
            JS_FreeCString(ctx, s);
            return JS_EXCEPTION;
        }
        JS_FreeCString(ctx, s);
    }
    if (mxt_vec(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, fn, "pvalues", &p, &n))
        return JS_EXCEPTION;
    out = (double*)malloc((size_t)(n ? n : 1) * sizeof(double));
    if (!out) {
        free(p);
        return JS_ThrowOutOfMemory(ctx);
    }
    rc = dyn_st_p_adjust(p, n, (dyn_st_adjust_t)method, out);
    free(p);
    if (rc != DYN_ST_OK) {
        free(out);
        return mxt_throw(ctx, fn, rc);
    }
    return mxt_f64(ctx, out, n);
}

static JSValue mxt_psi(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const keys[] = { "eps", "bins" };
    const char* fn = magic ? "stats.psiCounts" : "stats.psi";
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    double *e, *a, eps, bins = 10.0, out = 0.0;
    uint32_t ne, na;
    int rc;
    (void)this_val;
    if (mxt_opts_check(ctx, opts, fn, keys, magic ? 1 : 2) || mxt_num(ctx, opts, fn, "eps", 1e-4, &eps)
        || (!magic && mxt_num(ctx, opts, fn, "bins", 10.0, &bins)))
        return JS_EXCEPTION;
    if (!(eps > 0.0 && eps < 1.0))
        return JS_ThrowRangeError(ctx, "%s: eps must be in (0, 1)", fn);
    if (!(bins >= 2.0 && bins <= 65536.0) || bins != floor(bins))
        return JS_ThrowRangeError(ctx, "%s: bins must be an integer in [2, 65536]", fn);
    if (mxt_vec(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, fn, "expected", &e, &ne))
        return JS_EXCEPTION;
    if (mxt_vec(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, fn, "actual", &a, &na)) {
        free(e);
        return JS_EXCEPTION;
    }
    if (magic && ne != na) {
        free(e);
        free(a);
        return JS_ThrowRangeError(ctx, "%s: length mismatch (%u vs %u)", fn, ne, na);
    }
    rc = magic ? dyn_st_psi_counts(e, a, ne, eps, &out) : dyn_st_psi(e, ne, a, na, (uint32_t)bins, eps, &out);
    free(e);
    free(a);
    if (rc != DYN_ST_OK)
        return mxt_throw(ctx, fn, rc);
    return JS_NewFloat64(ctx, out);
}

typedef struct {
    JSValue a, opts;
    double *x, *y;
    uint32_t nx, ny;
    dyn_st_boot_t o;
    int rc;
} mxt_boot_job_t;

static int mxt_boot_stop(void* ud)
{
    (void)ud;
    return dyn_fiber_cancel_state() > 0;
}

static int mxt_boot_parse(JSContext* ctx, mxt_boot_job_t* j)
{
    static const char* const keys[] = { "b", "statistic", "compare", "resamples", "level", "seed" };
    static const char* const stats_n[] = { "mean", "median" };
    static const char* const cmp_n[] = { "diff", "ratio" };
    const char* fn = "stats.bootstrap";
    JSValue b;
    double res, seed;
    int st, cmp;
    if (mxt_opts_check(ctx, j->opts, fn, keys, 6) || mxt_enum(ctx, j->opts, fn, "statistic", stats_n, 2, 0, &st)
        || mxt_enum(ctx, j->opts, fn, "compare", cmp_n, 2, 0, &cmp)
        || mxt_num(ctx, j->opts, fn, "resamples", 2000.0, &res) || mxt_num(ctx, j->opts, fn, "level", 0.95, &j->o.level)
        || mxt_num(ctx, j->opts, fn, "seed", 0.0, &seed))
        return -1;
    if (!(res >= 2.0 && res <= 16777216.0) || res != floor(res)) {
        JS_ThrowRangeError(ctx, "%s: resamples must be an integer in [2, 16777216]", fn);
        return -1;
    }
    if (!(j->o.level > 0.0 && j->o.level < 1.0)) {
        JS_ThrowRangeError(ctx, "%s: level must be in (0, 1)", fn);
        return -1;
    }
    if (!(seed >= 0.0 && seed <= 9007199254740991.0) || seed != floor(seed)) {
        JS_ThrowRangeError(ctx, "%s: seed must be a safe non-negative integer", fn);
        return -1;
    }
    j->o.stat = (dyn_st_boot_stat_t)st;
    j->o.cmp = (dyn_st_boot_cmp_t)cmp;
    j->o.resamples = (uint32_t)res;
    j->o.seed = (uint64_t)seed;
    j->o.stop = mxt_boot_stop;
    j->o.ud = NULL;
    if (mxt_vec(ctx, j->a, fn, "a", &j->x, &j->nx))
        return -1;
    if (mxt_get(ctx, j->opts, "b", &b))
        return -1;
    if (!JS_IsUndefined(b)) {
        int rc = mxt_vec(ctx, b, fn, "b", &j->y, &j->ny);
        JS_FreeValue(ctx, b);
        if (rc)
            return -1;
    }
    return 0;
}

static JSValue mxt_boot_entry(JSContext* ctx, void* arg)
{
    mxt_boot_job_t* j = (mxt_boot_job_t*)arg;
    JSValue res = JS_EXCEPTION;
    if (mxt_boot_parse(ctx, j) == 0) {
        if ((uint64_t)j->o.resamples * ((uint64_t)j->nx + j->ny) >= (1u << 18))
            dyn_fiber_offload();
        j->rc = dyn_st_bootstrap(j->x, j->nx, j->y, j->ny, &j->o);
        dyn_fiber_onjs();
        if (j->rc != DYN_ST_OK) {
            res = mxt_throw(ctx, "stats.bootstrap", j->rc);
        } else {
            res = JS_NewObject(ctx);
            if (!JS_IsException(res)
                && (JS_SetPropertyStr(ctx, res, "estimate", JS_NewFloat64(ctx, j->o.estimate)) < 0
                    || JS_SetPropertyStr(ctx, res, "low", JS_NewFloat64(ctx, j->o.low)) < 0
                    || JS_SetPropertyStr(ctx, res, "high", JS_NewFloat64(ctx, j->o.high)) < 0)) {
                JS_FreeValue(ctx, res);
                res = JS_EXCEPTION;
            }
        }
    }
    free(j->x);
    free(j->y);
    JS_FreeValue(ctx, j->a);
    JS_FreeValue(ctx, j->opts);
    free(j);
    return res;
}

static JSValue mxt_bootstrap(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    mxt_boot_job_t* j = (mxt_boot_job_t*)calloc(1, sizeof(*j));
    JSValue p;
    (void)this_val;
    if (!j)
        return JS_ThrowOutOfMemory(ctx);
    j->a = JS_DupValue(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    j->opts = JS_DupValue(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    p = dyn_fiber_start(ctx, mxt_boot_entry, j);
    if (JS_IsException(p)) {
        JS_FreeValue(ctx, j->a);
        JS_FreeValue(ctx, j->opts);
        free(j);
    }
    return p;
}

static const JSCFunctionListEntry mxt_funcs[] = {
    JS_CFUNC_MAGIC_DEF("normPdf", 1, mxt_dist, 0),
    JS_CFUNC_MAGIC_DEF("normCdf", 1, mxt_dist, 1),
    JS_CFUNC_MAGIC_DEF("normSf", 1, mxt_dist, 2),
    JS_CFUNC_MAGIC_DEF("normPpf", 1, mxt_dist, 3),
    JS_CFUNC_MAGIC_DEF("tPdf", 2, mxt_dist, 4),
    JS_CFUNC_MAGIC_DEF("tCdf", 2, mxt_dist, 5),
    JS_CFUNC_MAGIC_DEF("tSf", 2, mxt_dist, 6),
    JS_CFUNC_MAGIC_DEF("tPpf", 2, mxt_dist, 7),
    JS_CFUNC_MAGIC_DEF("chi2Pdf", 2, mxt_dist, 8),
    JS_CFUNC_MAGIC_DEF("chi2Cdf", 2, mxt_dist, 9),
    JS_CFUNC_MAGIC_DEF("chi2Sf", 2, mxt_dist, 10),
    JS_CFUNC_MAGIC_DEF("chi2Ppf", 2, mxt_dist, 11),
    JS_CFUNC_MAGIC_DEF("fCdf", 3, mxt_dist, 12),
    JS_CFUNC_MAGIC_DEF("fSf", 3, mxt_dist, 13),
    JS_CFUNC_MAGIC_DEF("fPpf", 3, mxt_dist, 14),
    JS_CFUNC_MAGIC_DEF("binomPmf", 3, mxt_dist, 15),
    JS_CFUNC_MAGIC_DEF("binomCdf", 3, mxt_dist, 16),
    JS_CFUNC_MAGIC_DEF("binomSf", 3, mxt_dist, 17),
    JS_CFUNC_MAGIC_DEF("poissonPmf", 2, mxt_dist, 18),
    JS_CFUNC_MAGIC_DEF("poissonCdf", 2, mxt_dist, 19),
    JS_CFUNC_MAGIC_DEF("poissonSf", 2, mxt_dist, 20),
    JS_CFUNC_MAGIC_DEF("kolmogorovSf", 1, mxt_dist, 21),
    JS_CFUNC_MAGIC_DEF("ttest1samp", 2, mxt_two, MXT_T1),
    JS_CFUNC_MAGIC_DEF("ttestInd", 2, mxt_two, MXT_TIND),
    JS_CFUNC_MAGIC_DEF("ttestRel", 2, mxt_two, MXT_TREL),
    JS_CFUNC_MAGIC_DEF("mannWhitneyU", 2, mxt_two, MXT_MWU),
    JS_CFUNC_MAGIC_DEF("ks2Samp", 2, mxt_two, MXT_KS2),
    JS_CFUNC_MAGIC_DEF("pearson", 2, mxt_two, MXT_PEARSON),
    JS_CFUNC_MAGIC_DEF("spearman", 2, mxt_two, MXT_SPEARMAN),
    JS_CFUNC_DEF("proportionsZTest", 4, mxt_ztest),
    JS_CFUNC_DEF("chiSquare", 1, mxt_chisquare),
    JS_CFUNC_DEF("chi2Contingency", 1, mxt_contingency),
    JS_CFUNC_DEF("rankData", 1, mxt_rankdata),
    JS_CFUNC_DEF("pAdjust", 1, mxt_padjust),
    JS_CFUNC_MAGIC_DEF("psi", 2, mxt_psi, 0),
    JS_CFUNC_MAGIC_DEF("psiCounts", 2, mxt_psi, 1),
    JS_CFUNC_DEF("bootstrap", 1, mxt_bootstrap),
};

int dyn_mathx_tests_register(JSContext* ctx, JSValue stats)
{
    return JS_SetPropertyFunctionList(ctx, stats, mxt_funcs, countof(mxt_funcs));
}

#endif
