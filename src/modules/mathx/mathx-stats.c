#include "mathx-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_MATHX)

#define DYN_MATHX_MAX_ELEMS (1u << 24)

typedef struct {
    double s, c;
} mxst_neumaier;

#define MXST_NEUMAIER_ZERO { 0.0, 0.0 }

static inline void mxst_neumaier_add(mxst_neumaier* k, double x)
{
    double t = k->s + x;
    if (fabs(k->s) >= fabs(x))
        k->c += (k->s - t) + x;
    else
        k->c += (x - t) + k->s;
    k->s = t;
}

static inline double mxst_neumaier_total(const mxst_neumaier* k)
{
    return k->s + k->c;
}

#define MXST_SELECT_SMALL 24

static int js_mathx_len(JSContext* ctx, JSValueConst v, uint32_t* plen)
{
    JSValue lv;
    if (!JS_IsObject(v)) {
        JS_ThrowTypeError(ctx, "expected an array");
        return -1;
    }
    lv = JS_GetPropertyStr(ctx, v, "length");
    if (JS_IsException(lv))
        return -1;
    if (JS_ToUint32(ctx, plen, lv)) {
        JS_FreeValue(ctx, lv);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    if (*plen > DYN_MATHX_MAX_ELEMS) {
        JS_ThrowRangeError(ctx, "mathx: array-like length above 16777216 is not supported");
        return -1;
    }
    return 0;
}

JSClassID mxst_capture_f64_cid(JSContext* ctx)
{
    JSValueConst args[1];
    JSValue ta;
    JSClassID cid = 0;
    args[0] = JS_NewInt32(ctx, 0);
    ta = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_FLOAT64);
    if (!JS_IsException(ta))
        cid = JS_GetClassID(ta);
    JS_FreeValue(ctx, ta);
    return cid;
}

int mxst_src_of(JSContext* ctx, JSValueConst v, mxst_src* s)
{
    JSClassID cid;
    s->p = NULL;
    s->arr = v;
    JS_GetAnyOpaque(v, &cid);
    if (cid && cid == (JSClassID)mxst_cid_f64) {
        JSValue buf;
        uint8_t* base;
        size_t off, len, bpe, ab;
        buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
        if (JS_IsException(buf))
            return -1;
        base = JS_GetArrayBuffer(ctx, &ab, buf);
        JS_FreeValue(ctx, buf);
        if (!base) {
            JS_ThrowTypeError(ctx, "typed array buffer is detached");
            return -1;
        }
        if (off > ab || len > ab - off) {
            JS_ThrowRangeError(ctx, "typed array out of bounds");
            return -1;
        }
        s->p = (const double*)(base + off);
        s->len = (uint32_t)(len / 8);
        return 0;
    }
    return js_mathx_len(ctx, v, &s->len) < 0 ? -1 : 0;
}

static const char* const mxst_pop_keys[] = { "pop" };

static int mxst_read_pop(JSContext* ctx, JSValueConst opts)
{
    JSValue p;
    int b;
    if (JS_IsUndefined(opts) || JS_IsNull(opts))
        return 0;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "opts must be an object or undefined");
        return -1;
    }
    if (dyn_opts_strict(ctx, opts, mxst_pop_keys, countof(mxst_pop_keys)) < 0)
        return -1;
    p = JS_GetPropertyStr(ctx, opts, "pop");
    if (JS_IsException(p))
        return -1;
    if (JS_IsUndefined(p))
        b = 0;
    else
        b = JS_ToBool(ctx, p);
    JS_FreeValue(ctx, p);
    return b;
}

static int mxst_fold_sum(JSContext* ctx, const mxst_src* s, double* out)
{
    mxst_neumaier k = MXST_NEUMAIER_ZERO;
    uint32_t i;
    if (s->p) {
        for (i = 0; i < s->len; i++)
            mxst_neumaier_add(&k, s->p[i]);
    } else {
        for (i = 0; i < s->len; i++) {
            double x;
            if (mxst_elem(ctx, s->arr, i, &x))
                return -1;
            mxst_neumaier_add(&k, x);
        }
    }
    *out = mxst_neumaier_total(&k);
    return 0;
}

static int mxst_fold_var(JSContext* ctx, const mxst_src* s,
    double* pmean, double* pm2)
{
    mxst_neumaier k = MXST_NEUMAIER_ZERO, kd = MXST_NEUMAIER_ZERO;
    double mean, sd;
    uint32_t i, n = s->len;
    if (mxst_fold_sum(ctx, s, &mean))
        return -1;
    mean = mean / (double)n;
    if (s->p) {
        for (i = 0; i < n; i++) {
            double d = s->p[i] - mean;
            mxst_neumaier_add(&kd, d);
            mxst_neumaier_add(&k, d * d);
        }
    } else {
        for (i = 0; i < n; i++) {
            double x, d;
            if (mxst_elem(ctx, s->arr, i, &x))
                return -1;
            d = x - mean;
            mxst_neumaier_add(&kd, d);
            mxst_neumaier_add(&k, d * d);
        }
    }
    sd = mxst_neumaier_total(&kd);
    *pmean = mean;
    *pm2 = mxst_neumaier_total(&k) - sd * (sd / (double)n);
    if (*pm2 < 0.0)
        *pm2 = 0.0;
    return 0;
}

static double* mxst_materialize(JSContext* ctx, const mxst_src* s, int* pisnan);

static int mxst_fold_cov(JSContext* ctx, const mxst_src* a, const mxst_src* b,
    int want_m2, double* pcxy, double* pm2x, double* pm2y)
{
    mxst_neumaier kc = MXST_NEUMAIER_ZERO;
    mxst_neumaier kvx = MXST_NEUMAIER_ZERO, kvy = MXST_NEUMAIER_ZERO;
    mxst_neumaier kdx = MXST_NEUMAIER_ZERO, kdy = MXST_NEUMAIER_ZERO;
    double sdx, sdy;
    const double *xp, *yp;
    double *xa = NULL, *ya = NULL;
    mxst_src ta, tb;
    double mx, my;
    uint32_t i, n = a->len;
    if (a->p && b->p) {
        xp = a->p;
        yp = b->p;
    } else {
        int isn;
        mxst_src t;
        if (a->p) {
            size_t xnbytes = (size_t)n * sizeof(double);
            if (mxst_src_of(ctx, a->arr, &t))
                return -1;
            xa = (double*)dyn_nat_malloc(xnbytes);
            if (!xa) {
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            memcpy(xa, t.p, xnbytes);
        }
        if (b->p) {
            size_t ynbytes = (size_t)n * sizeof(double);
            if (mxst_src_of(ctx, b->arr, &t))
                return -1;
            ya = (double*)dyn_nat_malloc(ynbytes);
            if (!ya) {
                JS_ThrowOutOfMemory(ctx);
                dyn_nat_free(xa);
                return -1;
            }
            memcpy(ya, t.p, ynbytes);
        }
        if (!a->p) {
            xa = mxst_materialize(ctx, a, &isn);
            if (!xa) {
                dyn_nat_free(ya);
                return -1;
            }
        }
        if (!b->p) {
            ya = mxst_materialize(ctx, b, &isn);
            if (!ya) {
                dyn_nat_free(xa);
                return -1;
            }
        }
        xp = xa;
        yp = ya;
    }
    ta.p = xp;
    ta.arr = a->arr;
    ta.len = n;
    tb.p = yp;
    tb.arr = b->arr;
    tb.len = n;
    if (mxst_fold_sum(ctx, &ta, &mx) || mxst_fold_sum(ctx, &tb, &my))
        return -1;
    mx = mx / (double)n;
    my = my / (double)n;
    for (i = 0; i < n; i++) {
        double dx = xp[i] - mx, dy = yp[i] - my;
        mxst_neumaier_add(&kdx, dx);
        mxst_neumaier_add(&kdy, dy);
        mxst_neumaier_add(&kc, dx * dy);
        if (want_m2) {
            mxst_neumaier_add(&kvx, dx * dx);
            mxst_neumaier_add(&kvy, dy * dy);
        }
    }
    dyn_nat_free(xa);
    dyn_nat_free(ya);
    sdx = mxst_neumaier_total(&kdx);
    sdy = mxst_neumaier_total(&kdy);
    *pcxy = mxst_neumaier_total(&kc) - sdx * (sdy / (double)n);
    if (want_m2) {
        *pm2x = mxst_neumaier_total(&kvx) - sdx * (sdx / (double)n);
        *pm2y = mxst_neumaier_total(&kvy) - sdy * (sdy / (double)n);
        if (*pm2x < 0.0)
            *pm2x = 0.0;
        if (*pm2y < 0.0)
            *pm2y = 0.0;
    }
    return 0;
}

static double* mxst_materialize(JSContext* ctx, const mxst_src* s, int* pisnan)
{
    uint32_t i, n = s->len;
    size_t nbytes = (size_t)n * sizeof(double);
    double* v = (double*)dyn_nat_malloc(n ? nbytes : sizeof(double));
    if (!v) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    if (s->p) {
        memcpy(v, s->p, nbytes);
    } else {
        for (i = 0; i < n; i++) {
            if (mxst_elem(ctx, s->arr, i, &v[i])) {
                dyn_nat_free(v);
                return NULL;
            }
        }
    }
    for (i = 0; i < n; i++) {
        if (isnan(v[i])) {
            *pisnan = 1;
            return v;
        }
    }
    *pisnan = 0;
    return v;
}

static int mxst_cmp_double(const void* pa, const void* pb)
{
    double a = *(const double*)pa, b = *(const double*)pb;
    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0;
}

static void mxst_select(double* v, size_t n, size_t k)
{
    size_t lo = 0, hi = n - 1;
    unsigned rounds = 0, budget = 16;
    size_t span = n;

    while (span > 1) {
        span >>= 1;
        budget += 3;
    }
    while (hi - lo >= MXST_SELECT_SMALL) {
        size_t mid = lo + (hi - lo) / 2, i = lo, j = hi;
        double pivot, t;

        if (++rounds > budget) {
            qsort(v + lo, hi - lo + 1, sizeof(double), mxst_cmp_double);
            return;
        }
        if (v[mid] < v[lo]) {
            t = v[mid];
            v[mid] = v[lo];
            v[lo] = t;
        }
        if (v[hi] < v[lo]) {
            t = v[hi];
            v[hi] = v[lo];
            v[lo] = t;
        }
        if (v[hi] < v[mid]) {
            t = v[hi];
            v[hi] = v[mid];
            v[mid] = t;
        }
        pivot = v[mid];
        while (i <= j) {
            while (v[i] < pivot)
                i++;
            while (v[j] > pivot)
                j--;
            if (i <= j) {
                t = v[i];
                v[i] = v[j];
                v[j] = t;
                i++;
                if (j == 0)
                    break;
                j--;
            }
        }
        if (k <= j)
            hi = j;
        else if (k >= i)
            lo = i;
        else
            return;
    }
    {
        size_t a, b;
        for (a = lo + 1; a <= hi; a++) {
            double x = v[a];
            for (b = a; b > lo && v[b - 1] > x; b--)
                v[b] = v[b - 1];
            v[b] = x;
        }
    }
}

JSValue dyn_stats_sum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    mxst_src s;
    double v;
    (void)this_val;
    (void)argc;
    if (mxst_src_of(ctx, argv[0], &s))
        return JS_EXCEPTION;
    if (mxst_fold_sum(ctx, &s, &v))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, v);
}

JSValue dyn_stats_mean(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    mxst_src s;
    double v;
    (void)this_val;
    (void)argc;
    if (mxst_src_of(ctx, argv[0], &s))
        return JS_EXCEPTION;
    if (mxst_fold_sum(ctx, &s, &v))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, v / (double)s.len);
}

JSValue dyn_stats_var(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    mxst_src s;
    double mean, m2;
    int pop;
    (void)this_val;
    pop = mxst_read_pop(ctx, argc >= 2 ? argv[1] : JS_UNDEFINED);
    if (pop < 0)
        return JS_EXCEPTION;
    if (mxst_src_of(ctx, argv[0], &s))
        return JS_EXCEPTION;
    if (s.len == 0 || (pop == 0 && s.len == 1))
        return JS_NewFloat64(ctx, DYN_NAN);
    if (mxst_fold_var(ctx, &s, &mean, &m2))
        return JS_EXCEPTION;
    m2 = pop ? m2 / (double)s.len : m2 / (double)(s.len - 1);
    return JS_NewFloat64(ctx, magic ? sqrt(m2) : m2);
}

JSValue dyn_stats_quantile(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int median)
{
    static const char* const nm[2] = { "quantile", "median" };
    mxst_src s;
    double q = 0.5, pos, frac, res;
    double* v;
    int isn;
    uint32_t lo;
    (void)this_val;
    (void)argc;
    if (!median && JS_ToFloat64(ctx, &q, argv[1]))
        return JS_EXCEPTION;
    if (!median && (isnan(q) || q < 0.0 || q > 1.0))
        return JS_ThrowRangeError(ctx, "%s: q must be in [0, 1], got %g",
            nm[median], q);
    if (mxst_src_of(ctx, argv[0], &s))
        return JS_EXCEPTION;
    if (s.len == 0)
        return JS_ThrowRangeError(ctx, "%s of an empty array", nm[median]);
    v = mxst_materialize(ctx, &s, &isn);
    if (!v)
        return JS_EXCEPTION;
    if (isn) {
        dyn_nat_free(v);
        return JS_NewFloat64(ctx, DYN_NAN);
    }
    pos = q * (double)(s.len - 1);
    lo = (uint32_t)pos;
    frac = pos - (double)lo;
    mxst_select(v, s.len, lo);
    if (frac == 0.0 || lo + 1 >= s.len) {
        res = v[lo];
    } else {
        double next = v[lo + 1];
        size_t i;
        for (i = lo + 2; i < s.len; i++)
            if (v[i] < next)
                next = v[i];
        res = v[lo] + frac * (next - v[lo]);
    }
    dyn_nat_free(v);
    return JS_NewFloat64(ctx, res);
}

JSValue dyn_stats_minmax(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int ismax)
{
    mxst_src s;
    double best = ismax ? -DYN_INFINITY : DYN_INFINITY;
    uint32_t i;
    (void)this_val;
    (void)argc;
    if (mxst_src_of(ctx, argv[0], &s))
        return JS_EXCEPTION;
    if (s.p) {
        for (i = 0; i < s.len; i++) {
            double x = s.p[i];
            if (isnan(x))
                return JS_NewFloat64(ctx, DYN_NAN);
            if (ismax ? (x > best || (x == best && !signbit(x)))
                      : (x < best || (x == best && signbit(x))))
                best = x;
        }
    } else {
        for (i = 0; i < s.len; i++) {
            double x;
            if (mxst_elem(ctx, s.arr, i, &x))
                return JS_EXCEPTION;
            if (isnan(x))
                return JS_NewFloat64(ctx, DYN_NAN);
            if (ismax ? (x > best || (x == best && !signbit(x)))
                      : (x < best || (x == best && signbit(x))))
                best = x;
        }
    }
    return JS_NewFloat64(ctx, best);
}

JSValue dyn_stats_cov(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    mxst_src a, b;
    double cxy, mx, my;
    int pop;
    (void)this_val;
    pop = mxst_read_pop(ctx, argc >= 3 ? argv[2] : JS_UNDEFINED);
    if (pop < 0)
        return JS_EXCEPTION;
    if (mxst_src_of(ctx, argv[0], &a) || mxst_src_of(ctx, argv[1], &b))
        return JS_EXCEPTION;
    if (a.len != b.len)
        return JS_ThrowRangeError(ctx, "cov: length mismatch (%u vs %u)",
            a.len, b.len);
    if (a.len == 0 || (pop == 0 && a.len == 1))
        return JS_NewFloat64(ctx, DYN_NAN);
    if (mxst_fold_cov(ctx, &a, &b, 0, &cxy, &mx, &my))
        return JS_EXCEPTION;
    cxy = pop ? cxy / (double)a.len : cxy / (double)(a.len - 1);
    return JS_NewFloat64(ctx, cxy);
}

JSValue dyn_stats_corr(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    mxst_src a, b;
    double cxy, m2x, m2y, r;
    (void)this_val;
    (void)argc;
    if (mxst_src_of(ctx, argv[0], &a) || mxst_src_of(ctx, argv[1], &b))
        return JS_EXCEPTION;
    if (a.len != b.len)
        return JS_ThrowRangeError(ctx, "corr: length mismatch (%u vs %u)",
            a.len, b.len);
    if (a.len == 0)
        return JS_ThrowRangeError(ctx, "corr: zero variance (empty input)");
    if (mxst_fold_cov(ctx, &a, &b, 1, &cxy, &m2x, &m2y))
        return JS_EXCEPTION;
    if (isnan(m2x) || isnan(m2y))
        return JS_NewFloat64(ctx, DYN_NAN);
    if (!(m2x > 0.0) || !(m2y > 0.0))
        return JS_ThrowRangeError(ctx, "corr: zero variance (constant input)");
    r = cxy / (sqrt(m2x) * sqrt(m2y));
    if (r > 1.0)
        r = 1.0;
    if (r < -1.0)
        r = -1.0;
    return JS_NewFloat64(ctx, r);
}

#endif
