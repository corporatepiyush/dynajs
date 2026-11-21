#include "sigk-int.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>

static void sg_window_sym(dyn_sg_window_t kind, double beta, uint32_t m, double* w)
{
    uint32_t i;
    if (m == 0)
        return;
    if (m == 1) {
        w[0] = 1.0;
        return;
    }
    if (kind == DYN_SG_WIN_HANN || kind == DYN_SG_WIN_HAMMING || kind == DYN_SG_WIN_BLACKMAN) {
        double a0, a1, a2, step = 2.0 * M_PI / (double)(m - 1);
        if (kind == DYN_SG_WIN_HANN) {
            a0 = 0.5;
            a1 = 0.5;
            a2 = 0.0;
        } else if (kind == DYN_SG_WIN_HAMMING) {
            a0 = 0.54;
            a1 = 0.46;
            a2 = 0.0;
        } else {
            a0 = 0.42;
            a1 = 0.50;
            a2 = 0.08;
        }
        for (i = 0; i < m; i++) {
            double fac = i + 1 == m ? M_PI : -M_PI + (double)i * step;
            w[i] = a0 + a1 * cos(fac) + a2 * cos(2.0 * fac);
        }
        return;
    }
    if (kind == DYN_SG_WIN_BARTLETT) {
        double h = (double)(m - 1) / 2.0;
        for (i = 0; i < m; i++)
            w[i] = (double)i <= h ? 2.0 * (double)i / (double)(m - 1) : 2.0 - 2.0 * (double)i / (double)(m - 1);
        return;
    }
    if (kind == DYN_SG_WIN_KAISER) {
        double alpha = (double)(m - 1) / 2.0, i0b = dyn_besseli_nu(0.0, beta);
        for (i = 0; i < m; i++) {
            double r = ((double)i - alpha) / alpha;
            w[i] = dyn_besseli_nu(0.0, beta * sqrt(fmax(0.0, 1.0 - r * r))) / i0b;
        }
        return;
    }
    for (i = 0; i < m; i++)
        w[i] = 1.0;
}

void dyn_sg_window(dyn_sg_window_t kind, double beta, uint32_t n, int symmetric, double* out)
{
    if (symmetric || n <= 1) {
        sg_window_sym(kind, beta, n, out);
        return;
    }
    {
        double* t = (double*)malloc((size_t)(n + 1) * sizeof(double));
        uint32_t i;
        if (!t) {
            for (i = 0; i < n; i++)
                out[i] = DYN_NAN;
            return;
        }
        sg_window_sym(kind, beta, n + 1, t);
        memcpy(out, t, (size_t)n * sizeof(double));
        free(t);
    }
}

double dyn_sg_kaiser_beta(double a)
{
    if (a > 50.0)
        return 0.1102 * (a - 8.7);
    if (a > 21.0)
        return 0.5842 * pow(a - 21.0, 0.4) + 0.07886 * (a - 21.0);
    return 0.0;
}

static double sg_sinc(double x)
{
    if (x == 0.0)
        return 1.0;
    return sin(M_PI * x) / (M_PI * x);
}

int dyn_sg_firwin(uint32_t numtaps, const double* cutoff, uint32_t ncut, int pass_zero, int scale, dyn_sg_window_t win,
    double beta, double* h)
{
    double edges[66], alpha = 0.5 * ((double)numtaps - 1.0), *w;
    uint32_t ne = 0, i, k;
    int pass_nyq = ((ncut & 1u) != 0) ^ (pass_zero != 0);
    if (numtaps == 0 || ncut == 0 || ncut > 64)
        return DYN_SG_E_SIZE;
    for (k = 0; k < ncut; k++)
        if (!(cutoff[k] > 0.0 && cutoff[k] < 1.0) || (k && !(cutoff[k] > cutoff[k - 1])))
            return DYN_SG_E_DOMAIN;
    if (pass_nyq && !(numtaps & 1u))
        return DYN_SG_E_DOMAIN;
    if (pass_zero)
        edges[ne++] = 0.0;
    for (k = 0; k < ncut; k++)
        edges[ne++] = cutoff[k];
    if (pass_nyq)
        edges[ne++] = 1.0;
    for (i = 0; i < numtaps; i++) {
        double m = (double)i - alpha, v = 0.0;
        for (k = 0; k + 1 < ne; k += 2)
            v += edges[k + 1] * sg_sinc(edges[k + 1] * m) - edges[k] * sg_sinc(edges[k] * m);
        h[i] = v;
    }
    w = (double*)malloc((size_t)numtaps * sizeof(double));
    if (!w)
        return DYN_SG_E_OOM;
    sg_window_sym(win, beta, numtaps, w);
    for (i = 0; i < numtaps; i++)
        h[i] *= w[i];
    free(w);
    if (scale) {
        double left = edges[0], right = edges[1], sf, s = 0.0;
        sf = left == 0.0 ? 0.0 : (right == 1.0 ? 1.0 : 0.5 * (left + right));
        for (i = 0; i < numtaps; i++)
            s += h[i] * cos(M_PI * ((double)i - alpha) * sf);
        for (i = 0; i < numtaps; i++)
            h[i] /= s;
    }
    return DYN_SG_OK;
}

int dyn_sg_lfilter(const double* b, uint32_t nb, const double* a, uint32_t na, const double* x, uint32_t n, double* y)
{
    uint32_t K = nb > na ? nb : na, i, k;
    double *bb, *aa, *z, a0;
    if (na == 0 || nb == 0)
        return DYN_SG_E_SIZE;
    a0 = a[0];
    if (a0 == 0.0 || !isfinite(a0))
        return DYN_SG_E_DOMAIN;
    bb = (double*)calloc((size_t)3 * K, sizeof(double));
    if (!bb)
        return DYN_SG_E_OOM;
    aa = bb + K;
    z = aa + K;
    for (k = 0; k < nb; k++)
        bb[k] = b[k] / a0;
    for (k = 0; k < na; k++)
        aa[k] = a[k] / a0;
    for (i = 0; i < n; i++) {
        double xi = x[i], yi = bb[0] * xi + z[0];
        for (k = 1; k < K; k++)
            z[k - 1] = bb[k] * xi + z[k] - aa[k] * yi;
        y[i] = yi;
    }
    free(bb);
    return DYN_SG_OK;
}

void dyn_sg_sosfilt(const double* sos, uint32_t nsec, double* x, uint32_t n)
{
    uint32_t s, i;
    for (s = 0; s < nsec; s++) {
        const double* c = sos + 6 * (size_t)s;
        double a0 = c[3], b0 = c[0] / a0, b1 = c[1] / a0, b2 = c[2] / a0, a1 = c[4] / a0, a2 = c[5] / a0;
        double z0 = 0.0, z1 = 0.0;
        for (i = 0; i < n; i++) {
            double xi = x[i], yi = b0 * xi + z0;
            z0 = b1 * xi + z1 - a1 * yi;
            z1 = b2 * xi - a2 * yi;
            x[i] = yi;
        }
    }
}

void dyn_sg_biquad(dyn_sg_biquad_t type, double w0, double q, double gain_db, double* o)
{
    double c = cos(w0), alpha = sin(w0) / (2.0 * q), A = pow(10.0, gain_db / 40.0), sa = 2.0 * sqrt(A) * alpha;
    double b0, b1, b2, a0, a1, a2;
    a0 = 1.0 + alpha;
    a1 = -2.0 * c;
    a2 = 1.0 - alpha;
    switch (type) {
    case DYN_SG_BQ_HIGHPASS:
        b0 = (1.0 + c) / 2.0;
        b1 = -(1.0 + c);
        b2 = b0;
        break;
    case DYN_SG_BQ_BANDPASS:
        b0 = alpha;
        b1 = 0.0;
        b2 = -alpha;
        break;
    case DYN_SG_BQ_NOTCH:
        b0 = 1.0;
        b1 = -2.0 * c;
        b2 = 1.0;
        break;
    case DYN_SG_BQ_ALLPASS:
        b0 = 1.0 - alpha;
        b1 = -2.0 * c;
        b2 = 1.0 + alpha;
        break;
    case DYN_SG_BQ_PEAKING:
        b0 = 1.0 + alpha * A;
        b1 = -2.0 * c;
        b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A;
        a2 = 1.0 - alpha / A;
        break;
    case DYN_SG_BQ_LOWSHELF:
        b0 = A * ((A + 1.0) - (A - 1.0) * c + sa);
        b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * c);
        b2 = A * ((A + 1.0) - (A - 1.0) * c - sa);
        a0 = (A + 1.0) + (A - 1.0) * c + sa;
        a1 = -2.0 * ((A - 1.0) + (A + 1.0) * c);
        a2 = (A + 1.0) + (A - 1.0) * c - sa;
        break;
    case DYN_SG_BQ_HIGHSHELF:
        b0 = A * ((A + 1.0) + (A - 1.0) * c + sa);
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * c);
        b2 = A * ((A + 1.0) + (A - 1.0) * c - sa);
        a0 = (A + 1.0) - (A - 1.0) * c + sa;
        a1 = 2.0 * ((A - 1.0) - (A + 1.0) * c);
        a2 = (A + 1.0) - (A - 1.0) * c - sa;
        break;
    case DYN_SG_BQ_LOWPASS:
    default:
        b0 = (1.0 - c) / 2.0;
        b1 = 1.0 - c;
        b2 = b0;
        break;
    }
    o[0] = b0 / a0;
    o[1] = b1 / a0;
    o[2] = b2 / a0;
    o[3] = 1.0;
    o[4] = a1 / a0;
    o[5] = a2 / a0;
}

typedef struct {
    double r, i;
} sg_c;

static sg_c sg_cmul(sg_c a, sg_c b)
{
    sg_c o = { a.r * b.r - a.i * b.i, a.r * b.i + a.i * b.r };
    return o;
}

static sg_c sg_cdiv(sg_c a, sg_c b)
{
    double d = b.r * b.r + b.i * b.i;
    sg_c o = { (a.r * b.r + a.i * b.i) / d, (a.i * b.r - a.r * b.i) / d };
    return o;
}

static sg_c sg_csqrt(sg_c a)
{
    double m = hypot(a.r, a.i), re = sqrt(0.5 * (m + a.r)), im = sqrt(0.5 * (m - a.r));
    sg_c o = { re, a.i < 0.0 ? -im : im };
    return o;
}

static uint32_t sg_cplxreal(const sg_c* v, uint32_t n, sg_c* out)
{
    uint32_t i, m = 0;
    for (i = 0; i < n; i++) {
        double mag = hypot(v[i].r, v[i].i);
        if (fabs(v[i].i) <= 100.0 * DBL_EPSILON * mag) {
            sg_c q = { v[i].r, 0.0 };
            out[m++] = q;
        } else if (v[i].i > 0.0) {
            out[m++] = v[i];
        }
    }
    return m;
}

static void sg_del(sg_c* v, uint32_t* n, uint32_t i)
{
    memmove(v + i, v + i + 1, (size_t)(*n - i - 1) * sizeof(sg_c));
    (*n)--;
}

static uint32_t sg_worst(const sg_c* p, uint32_t n, int real_only)
{
    uint32_t i, best = UINT32_MAX;
    double bd = 0.0;
    for (i = 0; i < n; i++) {
        double d;
        if (real_only && p[i].i != 0.0)
            continue;
        d = fabs(1.0 - hypot(p[i].r, p[i].i));
        if (best == UINT32_MAX || d < bd) {
            best = i;
            bd = d;
        }
    }
    return best;
}

static uint32_t sg_nearest(const sg_c* z, uint32_t n, sg_c to, int which)
{
    uint32_t i, best = UINT32_MAX;
    double bd = 0.0;
    for (i = 0; i < n; i++) {
        double d;
        if ((which == 1 && z[i].i != 0.0) || (which == 2 && z[i].i == 0.0))
            continue;
        d = hypot(z[i].r - to.r, z[i].i - to.i);
        if (best == UINT32_MAX || d < bd) {
            best = i;
            bd = d;
        }
    }
    return best;
}

static uint32_t sg_nreal(const sg_c* v, uint32_t n)
{
    uint32_t i, c = 0;
    for (i = 0; i < n; i++)
        c += v[i].i == 0.0;
    return c;
}

static void sg_quad(sg_c a, sg_c b, int pair, double* row)
{
    row[0] = 1.0;
    if (pair) {
        row[1] = -2.0 * a.r;
        row[2] = a.r * a.r + a.i * a.i;
    } else {
        row[1] = -(a.r + b.r);
        row[2] = a.r * b.r;
    }
}

static void sg_zpk2sos(const sg_c* zin, uint32_t nzin, const sg_c* pin, uint32_t npin, double k, double* sos,
    uint32_t ns)
{
    sg_c z[164], p[164], zero = { 0.0, 0.0 };
    sg_c za[164], pa[164];
    uint32_t nz, np, n = npin, si, i;
    for (i = 0; i < n; i++) {
        za[i] = zin[i];
        pa[i] = pin[i];
    }
    (void)nzin;
    if (n % 2 == 1) {
        za[n] = zero;
        pa[n] = zero;
        n++;
    }
    nz = sg_cplxreal(za, n, z);
    np = sg_cplxreal(pa, n, p);
    memset(sos, 0, (size_t)6 * ns * sizeof(double));
    for (si = ns; si-- > 0;) {
        double* row = sos + 6 * (size_t)si;
        uint32_t i1 = sg_worst(p, np, 0), j;
        sg_c p1 = p[i1], p2, z1, z2;
        sg_del(p, &np, i1);
        if (p1.i == 0.0 && sg_nreal(p, np) == 0) {
            j = sg_nearest(z, nz, p1, 1);
            z1 = z[j];
            sg_del(z, &nz, j);
            sg_quad(z1, zero, 0, row);
            sg_quad(p1, zero, 0, row + 3);
        } else if (np + 1 == nz && p1.i != 0.0 && sg_nreal(p, np) == 1 && sg_nreal(z, nz) == 1) {
            j = sg_nearest(z, nz, p1, 2);
            z1 = z[j];
            sg_del(z, &nz, j);
            sg_quad(z1, z1, 1, row);
            sg_quad(p1, p1, 1, row + 3);
        } else {
            int pc = p1.i != 0.0;
            if (!pc) {
                j = sg_worst(p, np, 1);
                p2 = p[j];
                sg_del(p, &np, j);
            } else {
                p2 = p1;
            }
            sg_quad(p1, p2, pc, row + 3);
            j = sg_nearest(z, nz, p1, 0);
            z1 = z[j];
            sg_del(z, &nz, j);
            if (z1.i != 0.0) {
                sg_quad(z1, z1, 1, row);
            } else {
                j = sg_nearest(z, nz, p1, 1);
                z2 = z[j];
                sg_del(z, &nz, j);
                sg_quad(z1, z2, 0, row);
            }
        }
    }
    for (i = 0; i < 3; i++)
        sos[i] *= k;
}

int dyn_sg_butter(uint32_t N, double w1, double w2, dyn_sg_btype_t bt, double* sos, uint32_t* nsec)
{
    sg_c p[80], z[80], pz[160], zz[160];
    uint32_t np = 0, nz = 0, k, ns;
    double kk = 1.0, wa, wb, wo, bw;
    if (N == 0 || N > 40)
        return DYN_SG_E_SIZE;
    if (!(w1 > 0.0 && w1 < 1.0))
        return DYN_SG_E_DOMAIN;
    if ((bt == DYN_SG_BANDPASS || bt == DYN_SG_BANDSTOP) && !(w2 > w1 && w2 < 1.0))
        return DYN_SG_E_DOMAIN;
    wa = 4.0 * tan(M_PI * w1 / 2.0);
    wb = 4.0 * tan(M_PI * w2 / 2.0);
    for (k = 0; k < N; k++) {
        double a = M_PI * (double)(2 * (int)k - (int)N + 1) / (2.0 * (double)N);
        sg_c q = { -cos(a), -sin(a) };
        p[k] = q;
    }
    if (bt == DYN_SG_LOWPASS) {
        for (k = 0; k < N; k++) {
            pz[k].r = p[k].r * wa;
            pz[k].i = p[k].i * wa;
        }
        np = N;
        kk = pow(wa, (double)N);
    } else if (bt == DYN_SG_HIGHPASS) {
        sg_c prod = { 1.0, 0.0 }, one = { 1.0, 0.0 }, wv = { wa, 0.0 };
        for (k = 0; k < N; k++) {
            sg_c mp = { -p[k].r, -p[k].i };
            pz[k] = sg_cdiv(wv, p[k]);
            prod = sg_cmul(prod, mp);
            zz[nz].r = 0.0;
            zz[nz++].i = 0.0;
        }
        np = N;
        kk = sg_cdiv(one, prod).r;
    } else {
        wo = sqrt(wa * wb);
        bw = wb - wa;
        if (bt == DYN_SG_BANDPASS) {
            for (k = 0; k < N; k++) {
                sg_c pl = { p[k].r * bw / 2.0, p[k].i * bw / 2.0 }, d = sg_cmul(pl, pl), s;
                d.r -= wo * wo;
                s = sg_csqrt(d);
                pz[np].r = pl.r + s.r;
                pz[np++].i = pl.i + s.i;
                pz[np].r = pl.r - s.r;
                pz[np++].i = pl.i - s.i;
                zz[nz].r = 0.0;
                zz[nz++].i = 0.0;
            }
            kk = pow(bw, (double)N);
        } else {
            sg_c prod = { 1.0, 0.0 }, one = { 1.0, 0.0 };
            for (k = 0; k < N; k++) {
                sg_c hb = { bw / 2.0, 0.0 }, ph = sg_cdiv(hb, p[k]), d = sg_cmul(ph, ph), s, mp = { -p[k].r, -p[k].i };
                d.r -= wo * wo;
                s = sg_csqrt(d);
                pz[np].r = ph.r + s.r;
                pz[np++].i = ph.i + s.i;
                pz[np].r = ph.r - s.r;
                pz[np++].i = ph.i - s.i;
                zz[nz].r = 0.0;
                zz[nz++].i = wo;
                zz[nz].r = 0.0;
                zz[nz++].i = -wo;
                prod = sg_cmul(prod, mp);
            }
            kk = sg_cdiv(one, prod).r;
        }
    }
    {
        sg_c num = { 1.0, 0.0 }, den = { 1.0, 0.0 };
        for (k = 0; k < nz; k++) {
            sg_c a = { 4.0 + zz[k].r, zz[k].i }, b = { 4.0 - zz[k].r, -zz[k].i };
            num = sg_cmul(num, b);
            z[k] = sg_cdiv(a, b);
        }
        for (k = 0; k < np; k++) {
            sg_c a = { 4.0 + pz[k].r, pz[k].i }, b = { 4.0 - pz[k].r, -pz[k].i };
            den = sg_cmul(den, b);
            pz[k] = sg_cdiv(a, b);
        }
        kk *= sg_cdiv(num, den).r;
        for (; nz < np; nz++) {
            z[nz].r = -1.0;
            z[nz].i = 0.0;
        }
    }
    ns = (np + 1) / 2;
    sg_zpk2sos(z, nz, pz, np, kk, sos, ns);
    *nsec = ns;
    return DYN_SG_OK;
}

static void sg_poly_at(const double* c, uint32_t n, double w, double* re, double* im)
{
    double er = cos(w), ei = -sin(w), ar = 0.0, ai = 0.0;
    uint32_t k = n;
    while (k-- > 0) {
        double tr = ar * er - ai * ei + c[k], ti = ar * ei + ai * er;
        ar = tr;
        ai = ti;
    }
    *re = ar;
    *im = ai;
}

void dyn_sg_freqz(const double* b, uint32_t nb, const double* a, uint32_t na, const double* w, uint32_t nw, double* hre,
    double* him)
{
    uint32_t i;
    for (i = 0; i < nw; i++) {
        double br, bi, ar = 1.0, ai = 0.0, d;
        sg_poly_at(b, nb, w[i], &br, &bi);
        if (na)
            sg_poly_at(a, na, w[i], &ar, &ai);
        d = ar * ar + ai * ai;
        hre[i] = (br * ar + bi * ai) / d;
        him[i] = (bi * ar - br * ai) / d;
    }
}

void dyn_sg_sosfreqz(const double* sos, uint32_t nsec, const double* w, uint32_t nw, double* hre, double* him)
{
    uint32_t i, s;
    for (i = 0; i < nw; i++) {
        double r = 1.0, im = 0.0;
        for (s = 0; s < nsec; s++) {
            double hr, hi, t;
            dyn_sg_freqz(sos + 6 * (size_t)s, 3, sos + 6 * (size_t)s + 3, 3, &w[i], 1, &hr, &hi);
            t = r * hr - im * hi;
            im = r * hi + im * hr;
            r = t;
        }
        hre[i] = r;
        him[i] = im;
    }
}
