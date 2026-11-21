#include "sigk-int.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define SG_MAX_FACTORS 40
#define SG_MAX_RADIX 31
#define SG_CACHE 16

typedef struct sg_plan {
    uint32_t n, nf, m;
    uint32_t f[SG_MAX_FACTORS];
    double* tw;
    double* rtw;
    struct sg_plan* sub;
    double* chirp;
    double* bfft;
    int refs;
    uint64_t used;
} sg_plan_t;

static pthread_mutex_t sg_lock = PTHREAD_MUTEX_INITIALIZER;
static sg_plan_t* sg_cache[SG_CACHE];
static uint64_t sg_clock;

static void sg_unit(uint64_t t, uint64_t n, double* c, double* s)
{
    double a = 2.0 * M_PI * (double)t / (double)n;
    *c = cos(a);
    *s = -sin(a);
}

static void sg_plan_put(struct sg_plan* p);

static void sg_plan_free(sg_plan_t* p)
{
    if (!p)
        return;
    free(p->tw);
    free(p->rtw);
    free(p->chirp);
    free(p->bfft);
    sg_plan_put(p->sub);
    free(p);
}

static int sg_factor(uint32_t n, uint32_t* f, uint32_t* nf)
{
    uint32_t k = 0, r;
    while (n % 4 == 0) {
        f[k++] = 4;
        n /= 4;
    }
    while (n % 2 == 0) {
        f[k++] = 2;
        n /= 2;
    }
    for (r = 3; r <= SG_MAX_RADIX && n > 1; r += 2)
        while (n % r == 0) {
            f[k++] = r;
            n /= r;
        }
    *nf = k;
    return n == 1;
}

static void sg_stockham(const sg_plan_t* p, double* x, double* y);

static sg_plan_t* sg_plan_get(uint32_t n, int want_rtw);

static sg_plan_t* sg_plan_new(uint32_t n)
{
    sg_plan_t* p = (sg_plan_t*)calloc(1, sizeof(*p));
    uint32_t t;
    if (!p)
        return NULL;
    p->n = n;
    if (sg_factor(n, p->f, &p->nf)) {
        p->tw = (double*)malloc((size_t)2 * n * sizeof(double));
        if (!p->tw) {
            free(p);
            return NULL;
        }
        for (t = 0; t < n; t++)
            sg_unit(t, n, &p->tw[2 * t], &p->tw[2 * t + 1]);
        return p;
    }
    p->m = 1;
    while (p->m < 2 * n - 1)
        p->m <<= 1;
    p->sub = sg_plan_get(p->m, 0);
    p->chirp = (double*)malloc((size_t)2 * n * sizeof(double));
    p->bfft = (double*)calloc((size_t)2 * p->m, sizeof(double));
    if (!p->sub || !p->chirp || !p->bfft) {
        sg_plan_free(p);
        return NULL;
    }
    for (t = 0; t < n; t++) {
        uint64_t k2 = ((uint64_t)t * t) % ((uint64_t)2 * n);
        double a = M_PI * (double)k2 / (double)n;
        p->chirp[2 * t] = cos(a);
        p->chirp[2 * t + 1] = -sin(a);
    }
    for (t = 0; t < n; t++) {
        p->bfft[2 * t] = p->chirp[2 * t];
        p->bfft[2 * t + 1] = -p->chirp[2 * t + 1];
        if (t) {
            p->bfft[2 * (p->m - t)] = p->chirp[2 * t];
            p->bfft[2 * (p->m - t) + 1] = -p->chirp[2 * t + 1];
        }
    }
    {
        double* y = (double*)malloc((size_t)2 * p->m * sizeof(double));
        if (!y) {
            sg_plan_free(p);
            return NULL;
        }
        sg_stockham(p->sub, p->bfft, y);
        free(y);
    }
    return p;
}

static int sg_rtw_fill(sg_plan_t* p)
{
    uint32_t h = p->n, k;
    double* r = (double*)malloc((size_t)2 * (h + 1) * sizeof(double));
    if (!r)
        return -1;
    for (k = 0; k <= h; k++)
        sg_unit(k, (uint64_t)2 * h, &r[2 * k], &r[2 * k + 1]);
    p->rtw = r;
    return 0;
}

static sg_plan_t* sg_plan_get(uint32_t n, int want_rtw)
{
    sg_plan_t* p = NULL;
    int i, slot = -1;
    pthread_mutex_lock(&sg_lock);
    for (i = 0; i < SG_CACHE; i++)
        if (sg_cache[i] && sg_cache[i]->n == n) {
            p = sg_cache[i];
            break;
        }
    if (p) {
        p->refs++;
        p->used = ++sg_clock;
        if (want_rtw && !p->rtw && sg_rtw_fill(p) < 0) {
            p->refs--;
            p = NULL;
        }
        pthread_mutex_unlock(&sg_lock);
        return p;
    }
    pthread_mutex_unlock(&sg_lock);
    p = sg_plan_new(n);
    if (!p)
        return NULL;
    if (want_rtw && sg_rtw_fill(p) < 0) {
        sg_plan_free(p);
        return NULL;
    }
    p->refs = 1;
    pthread_mutex_lock(&sg_lock);
    p->used = ++sg_clock;
    for (i = 0; i < SG_CACHE; i++) {
        if (!sg_cache[i]) {
            slot = i;
            break;
        }
        if (sg_cache[i]->refs == 0 && (slot < 0 || sg_cache[i]->used < sg_cache[slot]->used))
            slot = i;
    }
    if (slot >= 0) {
        sg_plan_t* old = sg_cache[slot];
        p->refs++;
        sg_cache[slot] = p;
        pthread_mutex_unlock(&sg_lock);
        if (old)
            sg_plan_free(old);
        pthread_mutex_lock(&sg_lock);
        p->refs--;
    }
    pthread_mutex_unlock(&sg_lock);
    return p;
}

static void sg_plan_put(sg_plan_t* p)
{
    int i, cached = 0, last;
    if (!p)
        return;
    pthread_mutex_lock(&sg_lock);
    p->refs--;
    last = p->refs == 0;
    for (i = 0; i < SG_CACHE; i++)
        cached |= sg_cache[i] == p;
    pthread_mutex_unlock(&sg_lock);
    if (last && !cached)
        sg_plan_free(p);
}

static void sg_stockham(const sg_plan_t* p, double* x, double* y)
{
    const uint32_t N = p->n;
    const double* tw = p->tw;
    double *a = x, *b = y;
    uint32_t s = 1, len = N, fi;
    for (fi = 0; fi < p->nf; fi++) {
        const uint32_t r = p->f[fi], m = len / r;
        uint32_t pi, q;
        if (r == 4) {
            for (pi = 0; pi < m; pi++) {
                const uint64_t t1 = (uint64_t)pi * s;
                const double w1r = tw[2 * t1], w1i = tw[2 * t1 + 1];
                const double w2r = tw[4 * t1], w2i = tw[4 * t1 + 1];
                const double w3r = tw[6 * t1], w3i = tw[6 * t1 + 1];
                for (q = 0; q < s; q++) {
                    const double* a0 = a + 2 * (q + (size_t)s * pi);
                    const double* a1 = a0 + 2 * (size_t)s * m;
                    const double* a2 = a1 + 2 * (size_t)s * m;
                    const double* a3 = a2 + 2 * (size_t)s * m;
                    double* o = b + 2 * (q + (size_t)s * 4 * pi);
                    double s02r = a0[0] + a2[0], s02i = a0[1] + a2[1], d02r = a0[0] - a2[0], d02i = a0[1] - a2[1];
                    double s13r = a1[0] + a3[0], s13i = a1[1] + a3[1], d13r = a1[0] - a3[0], d13i = a1[1] - a3[1];
                    double c1r = d02r + d13i, c1i = d02i - d13r, c2r = s02r - s13r, c2i = s02i - s13i;
                    double c3r = d02r - d13i, c3i = d02i + d13r;
                    o[0] = s02r + s13r;
                    o[1] = s02i + s13i;
                    o[2 * s] = c1r * w1r - c1i * w1i;
                    o[2 * s + 1] = c1r * w1i + c1i * w1r;
                    o[4 * s] = c2r * w2r - c2i * w2i;
                    o[4 * s + 1] = c2r * w2i + c2i * w2r;
                    o[6 * s] = c3r * w3r - c3i * w3i;
                    o[6 * s + 1] = c3r * w3i + c3i * w3r;
                }
            }
        } else if (r == 2) {
            for (pi = 0; pi < m; pi++) {
                const uint64_t t1 = (uint64_t)pi * s;
                const double wr = tw[2 * t1], wi = tw[2 * t1 + 1];
                for (q = 0; q < s; q++) {
                    const double* a0 = a + 2 * (q + (size_t)s * pi);
                    const double* a1 = a0 + 2 * (size_t)s * m;
                    double* o = b + 2 * (q + (size_t)s * 2 * pi);
                    double dr = a0[0] - a1[0], di = a0[1] - a1[1];
                    o[0] = a0[0] + a1[0];
                    o[1] = a0[1] + a1[1];
                    o[2 * s] = dr * wr - di * wi;
                    o[2 * s + 1] = dr * wi + di * wr;
                }
            }
        } else if (r == 3) {
            const double h = 0.86602540378443864676;
            for (pi = 0; pi < m; pi++) {
                const uint64_t t1 = (uint64_t)pi * s;
                const double w1r = tw[2 * t1], w1i = tw[2 * t1 + 1];
                const double w2r = tw[4 * t1], w2i = tw[4 * t1 + 1];
                for (q = 0; q < s; q++) {
                    const double* a0 = a + 2 * (q + (size_t)s * pi);
                    const double* a1 = a0 + 2 * (size_t)s * m;
                    const double* a2 = a1 + 2 * (size_t)s * m;
                    double* o = b + 2 * (q + (size_t)s * 3 * pi);
                    double tr = a1[0] + a2[0], ti = a1[1] + a2[1];
                    double ur = a0[0] - 0.5 * tr, ui = a0[1] - 0.5 * ti;
                    double vr = h * (a1[0] - a2[0]), vi = h * (a1[1] - a2[1]);
                    double c1r = ur + vi, c1i = ui - vr, c2r = ur - vi, c2i = ui + vr;
                    o[0] = a0[0] + tr;
                    o[1] = a0[1] + ti;
                    o[2 * s] = c1r * w1r - c1i * w1i;
                    o[2 * s + 1] = c1r * w1i + c1i * w1r;
                    o[4 * s] = c2r * w2r - c2i * w2i;
                    o[4 * s + 1] = c2r * w2i + c2i * w2r;
                }
            }
        } else if (r == 5) {
            const double c1 = 0.30901699437494742410, c2 = -0.80901699437494742410;
            const double s1 = 0.95105651629515357212, s2 = 0.58778525229247312917;
            for (pi = 0; pi < m; pi++) {
                const uint64_t t1 = (uint64_t)pi * s;
                const double w1r = tw[2 * t1], w1i = tw[2 * t1 + 1];
                const double w2r = tw[4 * t1], w2i = tw[4 * t1 + 1];
                const double w3r = tw[6 * t1], w3i = tw[6 * t1 + 1];
                const double w4r = tw[8 * t1], w4i = tw[8 * t1 + 1];
                for (q = 0; q < s; q++) {
                    const double* a0 = a + 2 * (q + (size_t)s * pi);
                    const double* a1 = a0 + 2 * (size_t)s * m;
                    const double* a2 = a1 + 2 * (size_t)s * m;
                    const double* a3 = a2 + 2 * (size_t)s * m;
                    const double* a4 = a3 + 2 * (size_t)s * m;
                    double* o = b + 2 * (q + (size_t)s * 5 * pi);
                    double t1r = a1[0] + a4[0], t1i = a1[1] + a4[1], t3r = a1[0] - a4[0], t3i = a1[1] - a4[1];
                    double t2r = a2[0] + a3[0], t2i = a2[1] + a3[1], t4r = a2[0] - a3[0], t4i = a2[1] - a3[1];
                    double b1r = a0[0] + c1 * t1r + c2 * t2r, b1i = a0[1] + c1 * t1i + c2 * t2i;
                    double b2r = a0[0] + c2 * t1r + c1 * t2r, b2i = a0[1] + c2 * t1i + c1 * t2i;
                    double d1r = s1 * t3r + s2 * t4r, d1i = s1 * t3i + s2 * t4i;
                    double d2r = s2 * t3r - s1 * t4r, d2i = s2 * t3i - s1 * t4i;
                    double x1r = b1r + d1i, x1i = b1i - d1r, x4r = b1r - d1i, x4i = b1i + d1r;
                    double x2r = b2r + d2i, x2i = b2i - d2r, x3r = b2r - d2i, x3i = b2i + d2r;
                    o[0] = a0[0] + t1r + t2r;
                    o[1] = a0[1] + t1i + t2i;
                    o[2 * s] = x1r * w1r - x1i * w1i;
                    o[2 * s + 1] = x1r * w1i + x1i * w1r;
                    o[4 * s] = x2r * w2r - x2i * w2i;
                    o[4 * s + 1] = x2r * w2i + x2i * w2r;
                    o[6 * s] = x3r * w3r - x3i * w3i;
                    o[6 * s + 1] = x3r * w3i + x3i * w3r;
                    o[8 * s] = x4r * w4r - x4i * w4i;
                    o[8 * s + 1] = x4r * w4i + x4i * w4r;
                }
            }
        } else {
            double wr_[SG_MAX_RADIX], wi_[SG_MAX_RADIX], ar[SG_MAX_RADIX], ai[SG_MAX_RADIX];
            uint32_t j, k;
            const uint64_t step = N / r;
            for (j = 0; j < r; j++) {
                wr_[j] = tw[2 * (j * step)];
                wi_[j] = tw[2 * (j * step) + 1];
            }
            for (pi = 0; pi < m; pi++) {
                for (q = 0; q < s; q++) {
                    double* o = b + 2 * (q + (size_t)s * r * pi);
                    for (j = 0; j < r; j++) {
                        const double* aj = a + 2 * (q + (size_t)s * (pi + (size_t)j * m));
                        ar[j] = aj[0];
                        ai[j] = aj[1];
                    }
                    for (k = 0; k < r; k++) {
                        double cr = 0.0, ci = 0.0;
                        uint64_t t;
                        for (j = 0; j < r; j++) {
                            uint32_t e = (uint32_t)(((uint64_t)j * k) % r);
                            cr += ar[j] * wr_[e] - ai[j] * wi_[e];
                            ci += ar[j] * wi_[e] + ai[j] * wr_[e];
                        }
                        t = ((uint64_t)pi * k * s) % N;
                        o[2 * (size_t)s * k] = cr * tw[2 * t] - ci * tw[2 * t + 1];
                        o[2 * (size_t)s * k + 1] = cr * tw[2 * t + 1] + ci * tw[2 * t];
                    }
                }
            }
        }
        {
            double* t = a;
            a = b;
            b = t;
        }
        s *= r;
        len = m;
    }
    if (a != x)
        memcpy(x, a, (size_t)2 * N * sizeof(double));
}

static void sg_bluestein(const sg_plan_t* p, double* x, double* w)
{
    const uint32_t n = p->n, m = p->m;
    double *a = w, *y = w + 2 * (size_t)m;
    uint32_t k;
    double inv = 1.0 / (double)m;
    memset(a, 0, (size_t)2 * m * sizeof(double));
    for (k = 0; k < n; k++) {
        double cr = p->chirp[2 * k], ci = p->chirp[2 * k + 1];
        a[2 * k] = x[2 * k] * cr - x[2 * k + 1] * ci;
        a[2 * k + 1] = x[2 * k] * ci + x[2 * k + 1] * cr;
    }
    sg_stockham(p->sub, a, y);
    for (k = 0; k < m; k++) {
        double br = p->bfft[2 * k], bi = p->bfft[2 * k + 1], ar = a[2 * k], ai = a[2 * k + 1];
        a[2 * k] = ar * br - ai * bi;
        a[2 * k + 1] = -(ar * bi + ai * br);
    }
    sg_stockham(p->sub, a, y);
    for (k = 0; k < n; k++) {
        double cr = p->chirp[2 * k], ci = p->chirp[2 * k + 1];
        double vr = a[2 * k] * inv, vi = -a[2 * k + 1] * inv;
        x[2 * k] = vr * cr - vi * ci;
        x[2 * k + 1] = vr * ci + vi * cr;
    }
}

static int sg_exec(sg_plan_t* p, double* x)
{
    size_t ws = p->sub ? (size_t)4 * p->m : (size_t)2 * p->n;
    double* w = (double*)malloc(ws * sizeof(double));
    if (!w)
        return DYN_SG_E_OOM;
    if (p->sub)
        sg_bluestein(p, x, w);
    else
        sg_stockham(p, x, w);
    free(w);
    return DYN_SG_OK;
}

int dyn_sg_cfft(double* x, uint32_t n, int inverse)
{
    sg_plan_t* p;
    uint32_t k;
    int rc;
    if (n <= 1)
        return DYN_SG_OK;
    if (n > DYN_SG_MAX_N)
        return DYN_SG_E_SIZE;
    p = sg_plan_get(n, 0);
    if (!p)
        return DYN_SG_E_OOM;
    if (inverse)
        for (k = 0; k < n; k++)
            x[2 * k + 1] = -x[2 * k + 1];
    rc = sg_exec(p, x);
    if (inverse) {
        double inv = 1.0 / (double)n;
        for (k = 0; k < n; k++) {
            x[2 * k] *= inv;
            x[2 * k + 1] = -x[2 * k + 1] * inv;
        }
    }
    sg_plan_put(p);
    return rc;
}

int dyn_sg_fft(double* re, double* im, uint32_t n, int inverse)
{
    double* x;
    uint32_t k;
    int rc;
    if (n > DYN_SG_MAX_N)
        return DYN_SG_E_SIZE;
    x = (double*)malloc((size_t)2 * (n ? n : 1) * sizeof(double));
    if (!x)
        return DYN_SG_E_OOM;
    for (k = 0; k < n; k++) {
        x[2 * k] = re[k];
        x[2 * k + 1] = im[k];
    }
    rc = dyn_sg_cfft(x, n, inverse);
    for (k = 0; k < n && rc == DYN_SG_OK; k++) {
        re[k] = x[2 * k];
        im[k] = x[2 * k + 1];
    }
    free(x);
    return rc;
}

int dyn_sg_rfft(const double* xin, uint32_t n, double* re, double* im)
{
    uint32_t h = n / 2, k;
    double* z;
    int rc;
    if (n == 0)
        return DYN_SG_E_SIZE;
    if (n > DYN_SG_MAX_N)
        return DYN_SG_E_SIZE;
    if (n == 1) {
        re[0] = xin[0];
        im[0] = 0.0;
        return DYN_SG_OK;
    }
    if (n & 1) {
        z = (double*)malloc((size_t)2 * n * sizeof(double));
        if (!z)
            return DYN_SG_E_OOM;
        for (k = 0; k < n; k++) {
            z[2 * k] = xin[k];
            z[2 * k + 1] = 0.0;
        }
        rc = dyn_sg_cfft(z, n, 0);
        for (k = 0; k <= h && rc == DYN_SG_OK; k++) {
            re[k] = z[2 * k];
            im[k] = z[2 * k + 1];
        }
        im[0] = 0.0;
        free(z);
        return rc;
    }
    {
        sg_plan_t* p = sg_plan_get(h, 1);
        if (!p)
            return DYN_SG_E_OOM;
        z = (double*)malloc((size_t)2 * h * sizeof(double));
        if (!z) {
            sg_plan_put(p);
            return DYN_SG_E_OOM;
        }
        memcpy(z, xin, (size_t)n * sizeof(double));
        rc = h > 1 ? sg_exec(p, z) : DYN_SG_OK;
        for (k = 0; k <= h && rc == DYN_SG_OK; k++) {
            uint32_t a = k % h, b = (h - k) % h;
            double zr = z[2 * a], zi = z[2 * a + 1], cr = z[2 * b], ci = -z[2 * b + 1];
            double er = 0.5 * (zr + cr), ei = 0.5 * (zi + ci);
            double or_ = 0.5 * (zi - ci), oi = -0.5 * (zr - cr);
            double wr = p->rtw[2 * k], wi = p->rtw[2 * k + 1];
            re[k] = er + or_ * wr - oi * wi;
            im[k] = ei + or_ * wi + oi * wr;
        }
        im[0] = 0.0;
        im[h] = 0.0;
        free(z);
        sg_plan_put(p);
        return rc;
    }
}

int dyn_sg_irfft(const double* yre, const double* yim, uint32_t nin, uint32_t n, double* out)
{
    uint32_t h = n / 2, k, nk = n / 2 + 1;
    double* z;
    int rc;
    if (n == 0 || n > DYN_SG_MAX_N)
        return DYN_SG_E_SIZE;
    if (n == 1) {
        out[0] = nin ? yre[0] : 0.0;
        return DYN_SG_OK;
    }
    if (n & 1) {
        z = (double*)calloc((size_t)2 * n, sizeof(double));
        if (!z)
            return DYN_SG_E_OOM;
        for (k = 0; k < nk && k < nin; k++) {
            z[2 * k] = yre[k];
            z[2 * k + 1] = k ? yim[k] : 0.0;
            if (k) {
                z[2 * (n - k)] = yre[k];
                z[2 * (n - k) + 1] = -yim[k];
            }
        }
        rc = dyn_sg_cfft(z, n, 1);
        for (k = 0; k < n && rc == DYN_SG_OK; k++)
            out[k] = z[2 * k];
        free(z);
        return rc;
    }
    {
        sg_plan_t* p = sg_plan_get(h, 1);
        double inv;
        if (!p)
            return DYN_SG_E_OOM;
        z = (double*)malloc((size_t)2 * h * sizeof(double));
        if (!z) {
            sg_plan_put(p);
            return DYN_SG_E_OOM;
        }
        for (k = 0; k < h; k++) {
            uint32_t b = h - k;
            double xr = k < nin ? yre[k] : 0.0, xi = (k < nin && k) ? yim[k] : 0.0;
            double cr = b < nin ? yre[b] : 0.0, ci = (b < nin && b != h) ? -yim[b] : 0.0;
            double er = 0.5 * (xr + cr), ei = 0.5 * (xi + ci), dr = 0.5 * (xr - cr), di = 0.5 * (xi - ci);
            double wr = p->rtw[2 * k], wi = -p->rtw[2 * k + 1];
            double or_ = dr * wr - di * wi, oi = dr * wi + di * wr;
            z[2 * k] = er - oi;
            z[2 * k + 1] = ei + or_;
        }
        for (k = 0; k < h; k++)
            z[2 * k + 1] = -z[2 * k + 1];
        rc = h > 1 ? sg_exec(p, z) : DYN_SG_OK;
        inv = 1.0 / (double)h;
        for (k = 0; k < h && rc == DYN_SG_OK; k++) {
            out[2 * k] = z[2 * k] * inv;
            out[2 * k + 1] = -z[2 * k + 1] * inv;
        }
        free(z);
        sg_plan_put(p);
        return rc;
    }
}

uint32_t dyn_sg_next_fast(uint32_t n)
{
    uint64_t best = UINT64_MAX, p2, p3, p5;
    if (n <= 1)
        return 1;
    for (p5 = 1; p5 < (uint64_t)2 * n; p5 *= 5)
        for (p3 = p5; p3 < (uint64_t)2 * n; p3 *= 3) {
            p2 = p3;
            while (p2 < n)
                p2 *= 2;
            if (p2 < best)
                best = p2;
        }
    return best > UINT32_MAX ? UINT32_MAX : (uint32_t)best;
}
