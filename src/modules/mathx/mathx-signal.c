#include "mathx-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_MATHX)

#include "core/dyn-sigk.h"

static JSValue mxs_throw(JSContext* ctx, const char* fn, int rc)
{
    switch (rc) {
    case DYN_SG_E_OOM:
        return JS_ThrowOutOfMemory(ctx);
    case DYN_SG_E_SIZE:
        return JS_ThrowRangeError(ctx, "%s: length out of range", fn);
    default:
        return JS_ThrowRangeError(ctx, "%s: argument out of domain", fn);
    }
}

static JSValue mxs_pair(JSContext* ctx, double* re, double* im, uint32_t n)
{
    JSValue o, a, b;
    a = mxt_f64(ctx, re, n);
    if (JS_IsException(a)) {
        free(im);
        return a;
    }
    b = mxt_f64(ctx, im, n);
    if (JS_IsException(b)) {
        JS_FreeValue(ctx, a);
        return b;
    }
    o = JS_NewObject(ctx);
    if (JS_IsException(o) || JS_SetPropertyStr(ctx, o, "re", a) < 0) {
        JS_FreeValue(ctx, o);
        JS_FreeValue(ctx, b);
        if (!JS_IsException(o))
            return JS_EXCEPTION;
        JS_FreeValue(ctx, a);
        return JS_EXCEPTION;
    }
    if (JS_SetPropertyStr(ctx, o, "im", b) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static double* mxs_alloc(JSContext* ctx, uint32_t n)
{
    double* p = (double*)malloc((size_t)(n ? n : 1) * sizeof(double));
    if (!p)
        JS_ThrowOutOfMemory(ctx);
    return p;
}

static int mxs_len_arg(JSContext* ctx, JSValueConst v, const char* fn, const char* what, int optional, uint32_t def,
    uint32_t* out)
{
    double d;
    *out = def;
    if (optional && (JS_IsUndefined(v) || JS_IsNull(v)))
        return 0;
    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "%s: %s must be a number", fn, what);
        return -1;
    }
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!(d >= 1.0 && d <= (double)DYN_SG_MAX_N) || d != floor(d)) {
        JS_ThrowRangeError(ctx, "%s: %s must be an integer in [1, 67108864]", fn, what);
        return -1;
    }
    *out = (uint32_t)d;
    return 0;
}

static JSValue mxs_fft(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int inverse)
{
    const char* fn = inverse ? "signal.ifft" : "signal.fft";
    double *re = NULL, *im = NULL;
    uint32_t n, ni = 0;
    int rc;
    (void)this_val;
    if (mxt_vec(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, fn, "re", &re, &n))
        return JS_EXCEPTION;
    if (n > DYN_SG_MAX_N) {
        free(re);
        return mxs_throw(ctx, fn, DYN_SG_E_SIZE);
    }
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        if (mxt_vec(ctx, argv[1], fn, "im", &im, &ni)) {
            free(re);
            return JS_EXCEPTION;
        }
        if (ni != n) {
            free(re);
            free(im);
            return JS_ThrowRangeError(ctx, "%s: re and im lengths differ (%u vs %u)", fn, n, ni);
        }
    } else {
        im = (double*)calloc(n ? n : 1, sizeof(double));
        if (!im) {
            free(re);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    rc = dyn_sg_fft(re, im, n, inverse);
    if (rc != DYN_SG_OK) {
        free(re);
        free(im);
        return mxs_throw(ctx, fn, rc);
    }
    return mxs_pair(ctx, re, im, n);
}

static JSValue mxs_rfft(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* fn = "signal.rfft";
    double *x, *pad, *re, *im;
    uint32_t nx, n, h;
    int rc;
    (void)this_val;
    if (mxt_vec(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, fn, "x", &x, &nx))
        return JS_EXCEPTION;
    if (mxs_len_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, fn, "n", 1, nx, &n)) {
        free(x);
        return JS_EXCEPTION;
    }
    if (n == 0 || n > DYN_SG_MAX_N) {
        free(x);
        return mxs_throw(ctx, fn, DYN_SG_E_SIZE);
    }
    h = n / 2 + 1;
    pad = (double*)calloc(n, sizeof(double));
    re = (double*)malloc((size_t)h * sizeof(double));
    im = (double*)malloc((size_t)h * sizeof(double));
    if (!pad || !re || !im) {
        free(x);
        free(pad);
        free(re);
        free(im);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(pad, x, (size_t)(nx < n ? nx : n) * sizeof(double));
    free(x);
    rc = dyn_sg_rfft(pad, n, re, im);
    free(pad);
    if (rc != DYN_SG_OK) {
        free(re);
        free(im);
        return mxs_throw(ctx, fn, rc);
    }
    return mxs_pair(ctx, re, im, h);
}

static JSValue mxs_irfft(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* fn = "signal.irfft";
    double *re, *im, *out;
    uint32_t nr, ni, n;
    int rc;
    (void)this_val;
    if (mxt_vec(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, fn, "re", &re, &nr))
        return JS_EXCEPTION;
    if (mxt_vec(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, fn, "im", &im, &ni)) {
        free(re);
        return JS_EXCEPTION;
    }
    if (nr != ni || nr == 0) {
        free(re);
        free(im);
        return JS_ThrowRangeError(ctx, "%s: re and im must be non-empty and the same length (%u vs %u)", fn, nr, ni);
    }
    if (nr == 1 && (argc < 3 || JS_IsUndefined(argv[2]) || JS_IsNull(argv[2]))) {
        free(re);
        free(im);
        return JS_ThrowRangeError(ctx, "%s: a one-bin spectrum needs an explicit n", fn);
    }
    if (mxs_len_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, fn, "n", 1, 2 * (nr - 1), &n)) {
        free(re);
        free(im);
        return JS_EXCEPTION;
    }
    out = mxs_alloc(ctx, n);
    if (!out) {
        free(re);
        free(im);
        return JS_EXCEPTION;
    }
    rc = dyn_sg_irfft(re, im, nr, n, out);
    free(re);
    free(im);
    if (rc != DYN_SG_OK) {
        free(out);
        return mxs_throw(ctx, fn, rc);
    }
    return mxt_f64(ctx, out, n);
}

static JSValue mxs_fftfreq(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int real)
{
    const char* fn = real ? "signal.rfftfreq" : "signal.fftfreq";
    uint32_t n, m, k;
    double d = 1.0, *out;
    (void)this_val;
    if (mxs_len_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, fn, "n", 0, 0, &n))
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (!JS_IsNumber(argv[1]))
            return JS_ThrowTypeError(ctx, "%s: d must be a number", fn);
        if (JS_ToFloat64(ctx, &d, argv[1]))
            return JS_EXCEPTION;
        if (!(d > 0.0) || !isfinite(d))
            return JS_ThrowRangeError(ctx, "%s: d must be positive and finite", fn);
    }
    m = real ? n / 2 + 1 : n;
    out = mxs_alloc(ctx, m);
    if (!out)
        return JS_EXCEPTION;
    for (k = 0; k < m; k++) {
        double idx = real || k < (n + 1) / 2 ? (double)k : (double)k - (double)n;
        out[k] = idx / ((double)n * d);
    }
    return mxt_f64(ctx, out, m);
}

static const char* const mxs_win_names[] = { "boxcar", "hann", "hamming", "blackman", "bartlett", "kaiser" };

static int mxs_window_opt(JSContext* ctx, JSValueConst opts, const char* fn, const char* key, int def, int* win,
    double* beta)
{
    if (mxt_enum(ctx, opts, fn, key, mxs_win_names, 6, def, win) || mxt_num(ctx, opts, fn, "beta", DYN_NAN, beta))
        return -1;
    if (*win == DYN_SG_WIN_KAISER && !(*beta >= 0.0 && isfinite(*beta))) {
        JS_ThrowTypeError(ctx, "%s: the kaiser window needs a finite beta >= 0", fn);
        return -1;
    }
    return 0;
}

static JSValue mxs_get_window(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "sym", "beta" };
    const char* fn = "signal.getWindow";
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    const char* s;
    int win = -1, sym, k;
    double beta;
    uint32_t n;
    double* out;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "%s: name must be a string", fn);
    s = JS_ToCString(ctx, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    for (k = 0; k < 6; k++)
        if (strcmp(s, mxs_win_names[k]) == 0)
            win = k;
    if (win < 0) {
        JS_ThrowRangeError(ctx, "%s: unknown window '%s'", fn, s);
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, s);
    if (mxs_len_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, fn, "n", 0, 0, &n)
        || mxt_opts_check(ctx, opts, fn, keys, 2) || mxt_bool(ctx, opts, fn, "sym", 0, &sym)
        || mxt_num(ctx, opts, fn, "beta", DYN_NAN, &beta))
        return JS_EXCEPTION;
    if (win == DYN_SG_WIN_KAISER && !(beta >= 0.0 && isfinite(beta)))
        return JS_ThrowTypeError(ctx, "%s: the kaiser window needs a finite opts.beta >= 0", fn);
    out = mxs_alloc(ctx, n);
    if (!out)
        return JS_EXCEPTION;
    dyn_sg_window((dyn_sg_window_t)win, beta, n, sym, out);
    return mxt_f64(ctx, out, n);
}

static JSValue mxs_kaiser_beta(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double a;
    (void)this_val;
    if (argc < 1 || !JS_IsNumber(argv[0]))
        return JS_ThrowTypeError(ctx, "signal.kaiserBeta: attenuation must be a number");
    if (JS_ToFloat64(ctx, &a, argv[0]))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, dyn_sg_kaiser_beta(a));
}

static JSValue mxs_kaiser_ord(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* fn = "signal.kaiserOrd";
    double a, w, nt;
    JSValue o;
    (void)this_val;
    if (argc < 2 || !JS_IsNumber(argv[0]) || !JS_IsNumber(argv[1]))
        return JS_ThrowTypeError(ctx, "%s: ripple and width must be numbers", fn);
    if (JS_ToFloat64(ctx, &a, argv[0]) || JS_ToFloat64(ctx, &w, argv[1]))
        return JS_EXCEPTION;
    a = fabs(a);
    if (!(a >= 8.0) || !isfinite(a))
        return JS_ThrowRangeError(ctx, "%s: the ripple attenuation must be at least 8 dB", fn);
    if (!(w > 0.0 && w <= 1.0))
        return JS_ThrowRangeError(ctx, "%s: width must be in (0, 1] (a fraction of Nyquist)", fn);
    nt = ceil((a - 7.95) / 2.285 / (M_PI * w) + 1.0);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (JS_SetPropertyStr(ctx, o, "numtaps", JS_NewFloat64(ctx, nt)) < 0
        || JS_SetPropertyStr(ctx, o, "beta", JS_NewFloat64(ctx, dyn_sg_kaiser_beta(a))) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue mxs_firwin(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "window", "beta", "passZero", "scale", "fs" };
    const char* fn = "signal.firwin";
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    uint32_t nt, nc, k;
    double *cut = NULL, *h, fs, beta;
    int win, pz, sc, rc;
    (void)this_val;
    if (mxs_len_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, fn, "numtaps", 0, 0, &nt)
        || mxt_opts_check(ctx, opts, fn, keys, 5) || mxs_window_opt(ctx, opts, fn, "window", DYN_SG_WIN_HAMMING, &win, &beta)
        || mxt_bool(ctx, opts, fn, "passZero", 1, &pz) || mxt_bool(ctx, opts, fn, "scale", 1, &sc)
        || mxt_num(ctx, opts, fn, "fs", 2.0, &fs))
        return JS_EXCEPTION;
    if (!(fs > 0.0) || !isfinite(fs))
        return JS_ThrowRangeError(ctx, "%s: fs must be positive", fn);
    if (argc > 1 && JS_IsNumber(argv[1])) {
        cut = mxs_alloc(ctx, 1);
        if (!cut)
            return JS_EXCEPTION;
        nc = 1;
        if (JS_ToFloat64(ctx, &cut[0], argv[1])) {
            free(cut);
            return JS_EXCEPTION;
        }
    } else if (mxt_vec(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, fn, "cutoff", &cut, &nc)) {
        return JS_EXCEPTION;
    }
    for (k = 0; k < nc; k++)
        cut[k] /= fs / 2.0;
    h = mxs_alloc(ctx, nt);
    if (!h) {
        free(cut);
        return JS_EXCEPTION;
    }
    rc = dyn_sg_firwin(nt, cut, nc, pz, sc, (dyn_sg_window_t)win, beta, h);
    free(cut);
    if (rc != DYN_SG_OK) {
        free(h);
        if (rc == DYN_SG_E_DOMAIN)
            return JS_ThrowRangeError(ctx, "%s: cutoffs must increase strictly inside (0, fs/2), and a filter passing "
                                           "Nyquist needs an odd numtaps",
                fn);
        return mxs_throw(ctx, fn, rc);
    }
    return mxt_f64(ctx, h, nt);
}

static JSValue mxs_lfilter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* fn = "signal.lfilter";
    double *b, *a, *x, *y;
    uint32_t nb, na, n;
    int rc;
    (void)this_val;
    if (mxt_vec(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, fn, "b", &b, &nb))
        return JS_EXCEPTION;
    if (mxt_vec(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, fn, "a", &a, &na)) {
        free(b);
        return JS_EXCEPTION;
    }
    if (mxt_vec(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, fn, "x", &x, &n)) {
        free(b);
        free(a);
        return JS_EXCEPTION;
    }
    y = mxs_alloc(ctx, n);
    if (!y) {
        free(b);
        free(a);
        free(x);
        return JS_EXCEPTION;
    }
    rc = dyn_sg_lfilter(b, nb, a, na, x, n, y);
    free(b);
    free(a);
    free(x);
    if (rc != DYN_SG_OK) {
        free(y);
        if (rc == DYN_SG_E_DOMAIN)
            return JS_ThrowRangeError(ctx, "%s: a[0] must be a non-zero finite number", fn);
        return JS_ThrowRangeError(ctx, "%s: b and a must be non-empty", fn);
    }
    return mxt_f64(ctx, y, n);
}

static int mxs_sos_arg(JSContext* ctx, JSValueConst v, const char* fn, double** sos, uint32_t* nsec)
{
    uint32_t n, k;
    *sos = NULL;
    if (JS_IsArray(ctx, v)) {
        JSValue first = JS_GetPropertyUint32(ctx, v, 0);
        int rows;
        if (JS_IsException(first))
            return -1;
        rows = JS_IsObject(first);
        JS_FreeValue(ctx, first);
        if (rows) {
            JSValue lv = JS_GetPropertyStr(ctx, v, "length");
            uint32_t nr;
            if (JS_IsException(lv) || JS_ToUint32(ctx, &nr, lv)) {
                JS_FreeValue(ctx, lv);
                return -1;
            }
            JS_FreeValue(ctx, lv);
            if (nr == 0 || nr > 4096) {
                JS_ThrowRangeError(ctx, "%s: sos needs 1..4096 sections", fn);
                return -1;
            }
            *sos = (double*)malloc((size_t)6 * nr * sizeof(double));
            if (!*sos) {
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            for (k = 0; k < nr; k++) {
                JSValue row = JS_GetPropertyUint32(ctx, v, k);
                double* r;
                uint32_t rl;
                int bad;
                if (JS_IsException(row)) {
                    free(*sos);
                    *sos = NULL;
                    return -1;
                }
                bad = mxt_vec(ctx, row, fn, "sos row", &r, &rl);
                JS_FreeValue(ctx, row);
                if (bad || rl != 6) {
                    if (!bad) {
                        free(r);
                        JS_ThrowRangeError(ctx, "%s: sos row %u has %u coefficients, expected 6", fn, k, rl);
                    }
                    free(*sos);
                    *sos = NULL;
                    return -1;
                }
                memcpy(*sos + 6 * (size_t)k, r, 6 * sizeof(double));
                free(r);
            }
            n = 6 * nr;
            goto check;
        }
    }
    if (mxt_vec(ctx, v, fn, "sos", sos, &n))
        return -1;
    if (n == 0 || n % 6 != 0) {
        free(*sos);
        *sos = NULL;
        JS_ThrowRangeError(ctx, "%s: sos must hold 6 coefficients per section (got %u)", fn, n);
        return -1;
    }
check:
    for (k = 0; k < n / 6; k++)
        if ((*sos)[6 * k + 3] == 0.0 || !isfinite((*sos)[6 * k + 3])) {
            free(*sos);
            *sos = NULL;
            JS_ThrowRangeError(ctx, "%s: section %u has a0 = 0", fn, k);
            return -1;
        }
    *nsec = n / 6;
    return 0;
}

static JSValue mxs_sosfilt(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* fn = "signal.sosfilt";
    double *sos, *x;
    uint32_t ns, n;
    (void)this_val;
    if (mxs_sos_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, fn, &sos, &ns))
        return JS_EXCEPTION;
    if (mxt_vec(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, fn, "x", &x, &n)) {
        free(sos);
        return JS_EXCEPTION;
    }
    dyn_sg_sosfilt(sos, ns, x, n);
    free(sos);
    return mxt_f64(ctx, x, n);
}

static const char* const mxs_bq_names[] = { "lowpass", "highpass", "bandpass", "notch", "allpass", "peaking", "lowshelf",
    "highshelf" };

static JSValue mxs_biquad(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "q", "fs", "gainDb" };
    const char* fn = "signal.biquad";
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    const char* s;
    double f0, q, fs, g, *o;
    int type = -1, k;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "%s: type must be a string", fn);
    s = JS_ToCString(ctx, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    for (k = 0; k < 8; k++)
        if (strcmp(s, mxs_bq_names[k]) == 0)
            type = k;
    if (type < 0) {
        JS_ThrowRangeError(ctx, "%s: unknown type '%s'", fn, s);
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, s);
    if (argc < 2 || !JS_IsNumber(argv[1]))
        return JS_ThrowTypeError(ctx, "%s: f0 must be a number", fn);
    if (JS_ToFloat64(ctx, &f0, argv[1]) || mxt_opts_check(ctx, opts, fn, keys, 3)
        || mxt_num(ctx, opts, fn, "q", M_SQRT1_2, &q) || mxt_num(ctx, opts, fn, "fs", 2.0, &fs)
        || mxt_num(ctx, opts, fn, "gainDb", 0.0, &g))
        return JS_EXCEPTION;
    if (!(fs > 0.0) || !isfinite(fs) || !(f0 > 0.0 && f0 < fs / 2.0) || !(q > 0.0) || !isfinite(q) || !isfinite(g))
        return JS_ThrowRangeError(ctx, "%s: need 0 < f0 < fs/2, q > 0 and a finite gainDb", fn);
    o = mxs_alloc(ctx, 6);
    if (!o)
        return JS_EXCEPTION;
    dyn_sg_biquad((dyn_sg_biquad_t)type, 2.0 * M_PI * f0 / fs, q, g, o);
    return mxt_f64(ctx, o, 6);
}

static JSValue mxs_butter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "btype", "fs" };
    static const char* const bt_names[] = { "lowpass", "highpass", "bandpass", "bandstop" };
    const char* fn = "signal.butter";
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    double fs, w[2] = { 0.0, 0.0 }, *wv = NULL, *sos;
    uint32_t order, nw = 0, ns = 0;
    int bt, rc;
    (void)this_val;
    if (mxs_len_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, fn, "order", 0, 0, &order)
        || mxt_opts_check(ctx, opts, fn, keys, 2) || mxt_enum(ctx, opts, fn, "btype", bt_names, 4, 0, &bt)
        || mxt_num(ctx, opts, fn, "fs", 2.0, &fs))
        return JS_EXCEPTION;
    if (order > 40)
        return JS_ThrowRangeError(ctx, "%s: order must be in [1, 40]", fn);
    if (!(fs > 0.0) || !isfinite(fs))
        return JS_ThrowRangeError(ctx, "%s: fs must be positive", fn);
    if (argc > 1 && JS_IsNumber(argv[1])) {
        if (JS_ToFloat64(ctx, &w[0], argv[1]))
            return JS_EXCEPTION;
        nw = 1;
    } else {
        if (mxt_vec(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, fn, "wn", &wv, &nw))
            return JS_EXCEPTION;
        if (nw >= 1)
            w[0] = wv[0];
        if (nw >= 2)
            w[1] = wv[1];
        free(wv);
    }
    if (nw != (bt >= DYN_SG_BANDPASS ? 2u : 1u))
        return JS_ThrowRangeError(ctx, "%s: %s needs %s", fn, bt_names[bt],
            bt >= DYN_SG_BANDPASS ? "wn = [low, high]" : "a single cutoff wn");
    w[0] /= fs / 2.0;
    w[1] /= fs / 2.0;
    sos = mxs_alloc(ctx, 6 * order);
    if (!sos)
        return JS_EXCEPTION;
    rc = dyn_sg_butter(order, w[0], w[1], (dyn_sg_btype_t)bt, sos, &ns);
    if (rc != DYN_SG_OK) {
        free(sos);
        if (rc == DYN_SG_E_DOMAIN)
            return JS_ThrowRangeError(ctx, "%s: cutoffs must lie strictly inside (0, fs/2), low < high", fn);
        return mxs_throw(ctx, fn, rc);
    }
    return mxt_f64(ctx, sos, 6 * ns);
}

static int mxs_worn(JSContext* ctx, JSValueConst opts, const char* fn, double** w, uint32_t* nw, double* fs)
{
    static const char* const keys[] = { "n", "fs" };
    double n;
    uint32_t k;
    if (mxt_opts_check(ctx, opts, fn, keys, 2) || mxt_num(ctx, opts, fn, "n", 512.0, &n)
        || mxt_num(ctx, opts, fn, "fs", DYN_NAN, fs))
        return -1;
    if (!(n >= 1.0 && n <= 16777216.0) || n != floor(n)) {
        JS_ThrowRangeError(ctx, "%s: n must be an integer in [1, 16777216]", fn);
        return -1;
    }
    if (!isnan(*fs) && !(*fs > 0.0 && isfinite(*fs))) {
        JS_ThrowRangeError(ctx, "%s: fs must be positive", fn);
        return -1;
    }
    *nw = (uint32_t)n;
    *w = mxs_alloc(ctx, *nw);
    if (!*w)
        return -1;
    for (k = 0; k < *nw; k++)
        (*w)[k] = M_PI * (double)k / n;
    return 0;
}

static JSValue mxs_freq_result(JSContext* ctx, double* w, double* re, double* im, uint32_t nw, double fs)
{
    JSValue o, p;
    uint32_t k;
    if (!isnan(fs))
        for (k = 0; k < nw; k++)
            w[k] = w[k] * fs / (2.0 * M_PI);
    p = mxs_pair(ctx, re, im, nw);
    if (JS_IsException(p)) {
        free(w);
        return p;
    }
    o = mxt_f64(ctx, w, nw);
    if (JS_IsException(o) || JS_SetPropertyStr(ctx, p, "w", o) < 0) {
        JS_FreeValue(ctx, p);
        return JS_EXCEPTION;
    }
    return p;
}

static JSValue mxs_freqz(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int sos_form)
{
    const char* fn = sos_form ? "signal.sosfreqz" : "signal.freqz";
    double *b = NULL, *a = NULL, *w = NULL, *re, *im, fs;
    uint32_t nb = 0, na = 0, nw = 0;
    JSValueConst opts = argc > (sos_form ? 1 : 2) ? argv[sos_form ? 1 : 2] : JS_UNDEFINED;
    (void)this_val;
    if (sos_form) {
        if (mxs_sos_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, fn, &b, &nb))
            return JS_EXCEPTION;
    } else {
        if (mxt_vec(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, fn, "b", &b, &nb))
            return JS_EXCEPTION;
        if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])
            && mxt_vec(ctx, argv[1], fn, "a", &a, &na)) {
            free(b);
            return JS_EXCEPTION;
        }
    }
    if (mxs_worn(ctx, opts, fn, &w, &nw, &fs)) {
        free(b);
        free(a);
        return JS_EXCEPTION;
    }
    re = mxs_alloc(ctx, nw);
    im = re ? mxs_alloc(ctx, nw) : NULL;
    if (!re || !im) {
        free(b);
        free(a);
        free(w);
        free(re);
        return JS_EXCEPTION;
    }
    if (sos_form)
        dyn_sg_sosfreqz(b, nb, w, nw, re, im);
    else
        dyn_sg_freqz(b, nb, a, na, w, nw, re, im);
    free(b);
    free(a);
    return mxs_freq_result(ctx, w, re, im, nw, fs);
}

static JSValue mxs_conv(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int corr)
{
    static const char* const modes[] = { "full", "same", "valid" };
    const char* fn = corr ? "signal.correlate" : "signal.convolve";
    double *a, *b, *out;
    uint32_t na, nb, len, k;
    int mode = 0, rc;
    (void)this_val;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        const char* s;
        if (!JS_IsString(argv[2]))
            return JS_ThrowTypeError(ctx, "%s: mode must be a string", fn);
        s = JS_ToCString(ctx, argv[2]);
        if (!s)
            return JS_EXCEPTION;
        mode = -1;
        for (k = 0; k < 3; k++)
            if (strcmp(s, modes[k]) == 0)
                mode = (int)k;
        if (mode < 0) {
            JS_ThrowRangeError(ctx, "%s: unknown mode '%s'", fn, s);
            JS_FreeCString(ctx, s);
            return JS_EXCEPTION;
        }
        JS_FreeCString(ctx, s);
    }
    if (mxt_vec(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, fn, "a", &a, &na))
        return JS_EXCEPTION;
    if (mxt_vec(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, fn, "b", &b, &nb)) {
        free(a);
        return JS_EXCEPTION;
    }
    if (na == 0 || nb == 0) {
        free(a);
        free(b);
        return JS_ThrowRangeError(ctx, "%s: inputs must be non-empty", fn);
    }
    if (corr)
        for (k = 0; k < nb / 2; k++) {
            double t = b[k];
            b[k] = b[nb - 1 - k];
            b[nb - 1 - k] = t;
        }
    len = dyn_sg_conv_len(na, nb, (dyn_sg_mode_t)mode);
    out = mxs_alloc(ctx, len);
    if (!out) {
        free(a);
        free(b);
        return JS_EXCEPTION;
    }
    rc = dyn_sg_convolve(a, na, b, nb, (dyn_sg_mode_t)mode, out);
    free(a);
    free(b);
    if (rc != DYN_SG_OK) {
        free(out);
        return mxs_throw(ctx, fn, rc);
    }
    return mxt_f64(ctx, out, len);
}

static JSValue mxs_welch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "fs", "window", "beta", "nperseg", "noverlap", "nfft", "detrend", "scaling" };
    static const char* const scal[] = { "density", "spectrum" };
    const char* fn = "signal.welch";
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    dyn_sg_welch_t o;
    double nps, nov, nfft, *x, *pxx, *f;
    uint32_t n, nb, k;
    int win, sc, rc;
    JSValue dv, res, fv;
    (void)this_val;
    memset(&o, 0, sizeof o);
    if (mxt_opts_check(ctx, opts, fn, keys, 8) || mxt_num(ctx, opts, fn, "fs", 1.0, &o.fs)
        || mxs_window_opt(ctx, opts, fn, "window", DYN_SG_WIN_HANN, &win, &o.beta)
        || mxt_num(ctx, opts, fn, "nperseg", 256.0, &nps) || mxt_num(ctx, opts, fn, "noverlap", DYN_NAN, &nov)
        || mxt_num(ctx, opts, fn, "nfft", DYN_NAN, &nfft) || mxt_enum(ctx, opts, fn, "scaling", scal, 2, 0, &sc)
        || mxt_get(ctx, opts, "detrend", &dv))
        return JS_EXCEPTION;
    o.detrend = 1;
    if (!JS_IsUndefined(dv)) {
        if (JS_IsBool(dv) && !JS_ToBool(ctx, dv)) {
            o.detrend = 0;
        } else if (JS_IsString(dv)) {
            const char* s = JS_ToCString(ctx, dv);
            int ok = s && strcmp(s, "constant") == 0;
            if (s)
                JS_FreeCString(ctx, s);
            if (!ok) {
                JS_FreeValue(ctx, dv);
                return JS_ThrowRangeError(ctx, "%s: detrend must be 'constant' or false", fn);
            }
        } else {
            JS_FreeValue(ctx, dv);
            return JS_ThrowTypeError(ctx, "%s: detrend must be 'constant' or false", fn);
        }
        JS_FreeValue(ctx, dv);
    }
    if (!(o.fs > 0.0) || !isfinite(o.fs))
        return JS_ThrowRangeError(ctx, "%s: fs must be positive", fn);
    if (!(nps >= 1.0 && nps <= (double)DYN_SG_MAX_N) || nps != floor(nps))
        return JS_ThrowRangeError(ctx, "%s: nperseg must be a positive integer", fn);
    if (mxt_vec(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, fn, "x", &x, &n))
        return JS_EXCEPTION;
    if (n == 0) {
        free(x);
        return JS_ThrowRangeError(ctx, "%s: x must be non-empty", fn);
    }
    o.window = (dyn_sg_window_t)win;
    o.density = sc == 0;
    o.nperseg = (uint32_t)nps < n ? (uint32_t)nps : n;
    if (isnan(nov))
        o.noverlap = o.nperseg / 2;
    else if (!(nov >= 0.0 && nov < (double)o.nperseg) || nov != floor(nov)) {
        free(x);
        return JS_ThrowRangeError(ctx, "%s: noverlap must be an integer in [0, nperseg)", fn);
    } else
        o.noverlap = (uint32_t)nov;
    if (isnan(nfft))
        o.nfft = o.nperseg;
    else if (!(nfft >= (double)o.nperseg && nfft <= (double)DYN_SG_MAX_N) || nfft != floor(nfft)) {
        free(x);
        return JS_ThrowRangeError(ctx, "%s: nfft must be an integer >= nperseg", fn);
    } else
        o.nfft = (uint32_t)nfft;
    nb = o.nfft / 2 + 1;
    pxx = mxs_alloc(ctx, nb);
    f = pxx ? mxs_alloc(ctx, nb) : NULL;
    if (!pxx || !f) {
        free(x);
        free(pxx);
        return JS_EXCEPTION;
    }
    rc = dyn_sg_welch(x, n, &o, pxx);
    free(x);
    if (rc != DYN_SG_OK) {
        free(pxx);
        free(f);
        return mxs_throw(ctx, fn, rc);
    }
    for (k = 0; k < nb; k++)
        f[k] = (double)k * o.fs / (double)o.nfft;
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) {
        free(pxx);
        free(f);
        return res;
    }
    fv = mxt_f64(ctx, f, nb);
    if (JS_IsException(fv) || JS_SetPropertyStr(ctx, res, "f", fv) < 0) {
        free(pxx);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    fv = mxt_f64(ctx, pxx, nb);
    if (JS_IsException(fv) || JS_SetPropertyStr(ctx, res, "pxx", fv) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

static JSValue mxs_hilbert(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* fn = "signal.hilbert";
    double *x, *re, *im;
    uint32_t nx, n;
    int rc;
    (void)this_val;
    if (mxt_vec(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, fn, "x", &x, &nx))
        return JS_EXCEPTION;
    if (mxs_len_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, fn, "n", 1, nx, &n)) {
        free(x);
        return JS_EXCEPTION;
    }
    if (n == 0) {
        free(x);
        return JS_ThrowRangeError(ctx, "%s: x must be non-empty", fn);
    }
    re = mxs_alloc(ctx, n);
    im = re ? mxs_alloc(ctx, n) : NULL;
    if (!re || !im) {
        free(x);
        free(re);
        return JS_EXCEPTION;
    }
    rc = dyn_sg_hilbert(x, nx, n, re, im);
    free(x);
    if (rc != DYN_SG_OK) {
        free(re);
        free(im);
        return mxs_throw(ctx, fn, rc);
    }
    return mxs_pair(ctx, re, im, n);
}

static JSValue mxs_goertzel(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* fn = "signal.goertzel";
    double *x, f, fs = 1.0, re, im;
    uint32_t n;
    JSValue o;
    (void)this_val;
    if (argc < 2 || !JS_IsNumber(argv[1]) || (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNumber(argv[2])))
        return JS_ThrowTypeError(ctx, "%s: freq and fs must be numbers", fn);
    if (JS_ToFloat64(ctx, &f, argv[1]) || (argc > 2 && !JS_IsUndefined(argv[2]) && JS_ToFloat64(ctx, &fs, argv[2])))
        return JS_EXCEPTION;
    if (!(fs > 0.0) || !isfinite(fs) || !isfinite(f))
        return JS_ThrowRangeError(ctx, "%s: need a finite freq and fs > 0", fn);
    if (mxt_vec(ctx, argv[0], fn, "x", &x, &n))
        return JS_EXCEPTION;
    dyn_sg_goertzel(x, n, 2.0 * M_PI * f / fs, &re, &im);
    free(x);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (JS_SetPropertyStr(ctx, o, "re", JS_NewFloat64(ctx, re)) < 0
        || JS_SetPropertyStr(ctx, o, "im", JS_NewFloat64(ctx, im)) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static int mxs_set_num(JSContext* ctx, JSValue o, const char* k, double v)
{
    return JS_SetPropertyStr(ctx, o, k, JS_NewFloat64(ctx, v));
}

static JSValue mxs_bls(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "bins", "minPoints" };
    static const char* const names[] = { "power", "duration", "depth", "epoch" };
    const char* fn = "signal.bls";
    JSValueConst opts = argc > 4 ? argv[4] : JS_UNDEFINED;
    double *t = NULL, *y = NULL, *per = NULL, *dur = NULL, *out[4] = { NULL, NULL, NULL, NULL };
    double bins, minp, mean = 0.0, var = 0.0, sde = DYN_NAN;
    uint32_t n, ny, np = 0, nd = 0, i, k;
    int64_t bi = -1;
    dyn_sg_bls_t o;
    JSValue res = JS_UNDEFINED, best, v;
    int rc;
    (void)this_val;
    if (mxt_opts_check(ctx, opts, fn, keys, 2) || mxt_num(ctx, opts, fn, "bins", 400.0, &bins)
        || mxt_num(ctx, opts, fn, "minPoints", 5.0, &minp))
        return JS_EXCEPTION;
    if (!(bins >= 2.0 && bins <= (double)DYN_SG_BLS_MAX_BINS) || bins != floor(bins))
        return JS_ThrowRangeError(ctx, "%s: bins must be an integer in [2, 1048576]", fn);
    if (!(minp >= 1.0 && minp <= 4294967295.0) || minp != floor(minp))
        return JS_ThrowRangeError(ctx, "%s: minPoints must be a positive integer", fn);
    if (mxt_vec(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, fn, "t", &t, &n))
        return JS_EXCEPTION;
    if (mxt_vec(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, fn, "y", &y, &ny)
        || mxt_vec(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, fn, "periods", &per, &np)
        || mxt_vec(ctx, argc > 3 ? argv[3] : JS_UNDEFINED, fn, "durations", &dur, &nd))
        goto fail;
    if (n != ny) {
        JS_ThrowRangeError(ctx, "%s: t and y lengths differ", fn);
        goto fail;
    }
    if (n < 2 || (double)n < minp) {
        JS_ThrowRangeError(ctx, "%s: need at least max(2, minPoints) observations", fn);
        goto fail;
    }
    for (i = 0; i < n; i++)
        if (!isfinite(t[i]) || !isfinite(y[i])) {
            JS_ThrowRangeError(ctx, "%s: t and y must be finite", fn);
            goto fail;
        }
    if (np == 0 || nd == 0) {
        JS_ThrowRangeError(ctx, "%s: periods and durations must be non-empty", fn);
        goto fail;
    }
    for (i = 0; i < np; i++)
        if (!(per[i] > 0.0) || !isfinite(per[i])) {
            JS_ThrowRangeError(ctx, "%s: periods must be finite and > 0", fn);
            goto fail;
        }
    for (i = 0; i < nd; i++)
        if (!(dur[i] > 0.0) || !isfinite(dur[i])) {
            JS_ThrowRangeError(ctx, "%s: durations must be finite and > 0", fn);
            goto fail;
        }
    for (k = 0; k < 4; k++)
        if (!(out[k] = mxs_alloc(ctx, np)))
            goto fail;
    o.nbins = (uint32_t)bins;
    o.min_points = (uint32_t)minp;
    rc = dyn_sg_bls(t, y, n, per, np, dur, nd, &o, out[0], out[1], out[2], out[3]);
    if (rc != DYN_SG_OK) {
        mxs_throw(ctx, fn, rc);
        goto fail;
    }
    for (i = 0; i < np; i++) {
        mean += out[0][i];
        if (out[0][i] > 0.0 && (bi < 0 || out[0][i] > out[0][bi]))
            bi = i;
    }
    mean /= (double)np;
    for (i = 0; i < np; i++)
        var += (out[0][i] - mean) * (out[0][i] - mean);
    if (np > 1 && var > 0.0 && bi >= 0)
        sde = (out[0][bi] - mean) / sqrt(var / (double)(np - 1));
    best = JS_NewObject(ctx);
    if (JS_IsException(best))
        goto fail;
    if (JS_SetPropertyStr(ctx, best, "index", JS_NewInt64(ctx, bi)) < 0
        || mxs_set_num(ctx, best, "period", bi < 0 ? DYN_NAN : per[bi]) < 0
        || mxs_set_num(ctx, best, "duration", bi < 0 ? DYN_NAN : out[1][bi]) < 0
        || mxs_set_num(ctx, best, "depth", bi < 0 ? DYN_NAN : out[2][bi]) < 0
        || mxs_set_num(ctx, best, "epoch", bi < 0 ? DYN_NAN : out[3][bi]) < 0
        || mxs_set_num(ctx, best, "power", bi < 0 ? 0.0 : out[0][bi]) < 0
        || mxs_set_num(ctx, best, "sde", sde) < 0) {
        JS_FreeValue(ctx, best);
        goto fail;
    }
    free(t);
    free(y);
    free(per);
    free(dur);
    t = y = per = dur = NULL;
    res = JS_NewObject(ctx);
    if (JS_IsException(res) || JS_SetPropertyStr(ctx, res, "best", best) < 0) {
        if (JS_IsException(res))
            JS_FreeValue(ctx, best);
        goto fail;
    }
    for (k = 0; k < 4; k++) {
        v = mxt_f64(ctx, out[k], np);
        out[k] = NULL;
        if (JS_IsException(v) || JS_SetPropertyStr(ctx, res, names[k], v) < 0)
            goto fail;
    }
    return res;
fail:
    for (k = 0; k < 4; k++)
        free(out[k]);
    free(t);
    free(y);
    free(per);
    free(dur);
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry mxs_funcs[] = {
    JS_CFUNC_MAGIC_DEF("fft", 1, mxs_fft, 0),
    JS_CFUNC_MAGIC_DEF("ifft", 2, mxs_fft, 1),
    JS_CFUNC_DEF("rfft", 1, mxs_rfft),
    JS_CFUNC_DEF("irfft", 2, mxs_irfft),
    JS_CFUNC_MAGIC_DEF("fftfreq", 1, mxs_fftfreq, 0),
    JS_CFUNC_MAGIC_DEF("rfftfreq", 1, mxs_fftfreq, 1),
    JS_CFUNC_DEF("getWindow", 2, mxs_get_window),
    JS_CFUNC_DEF("kaiserBeta", 1, mxs_kaiser_beta),
    JS_CFUNC_DEF("kaiserOrd", 2, mxs_kaiser_ord),
    JS_CFUNC_DEF("firwin", 2, mxs_firwin),
    JS_CFUNC_DEF("lfilter", 3, mxs_lfilter),
    JS_CFUNC_DEF("sosfilt", 2, mxs_sosfilt),
    JS_CFUNC_DEF("biquad", 2, mxs_biquad),
    JS_CFUNC_DEF("butter", 2, mxs_butter),
    JS_CFUNC_MAGIC_DEF("freqz", 1, mxs_freqz, 0),
    JS_CFUNC_MAGIC_DEF("sosfreqz", 1, mxs_freqz, 1),
    JS_CFUNC_MAGIC_DEF("convolve", 2, mxs_conv, 0),
    JS_CFUNC_MAGIC_DEF("correlate", 2, mxs_conv, 1),
    JS_CFUNC_DEF("welch", 1, mxs_welch),
    JS_CFUNC_DEF("hilbert", 1, mxs_hilbert),
    JS_CFUNC_DEF("goertzel", 2, mxs_goertzel),
    JS_CFUNC_DEF("bls", 4, mxs_bls),
};

int dyn_mathx_signal_register(JSContext* ctx, JSValue sig)
{
    return JS_SetPropertyFunctionList(ctx, sig, mxs_funcs, countof(mxs_funcs));
}

#endif
