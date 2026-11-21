// covers: c:src/core/sigk
/*
 * test_sigk_core.c -- src/core/sigk (the dyna:mathx signal kernels) as a
 * standalone C library under ASan/UBSan.
 *
 * Checks: the complex FFT matches a long-double naive DFT for every n in
 * 1..160 and for sizes that take each path (radix 4/2/3, the generic odd
 * radix up to 31, Bluestein for larger primes, mixed products), forward and
 * inverse; rfft equals the first n/2+1 bins of the complex FFT and irfft
 * inverts it for even and odd n; more distinct sizes than the plan cache
 * holds are reused correctly after eviction, including from many threads at
 * once; welch is bit-identical on one thread and many; convolve's FFT path
 * agrees with the direct sum, including odd full lengths L whose (L-1)/2 is
 * itself 5-smooth (a transform one short would wrap); next_fast returns the smallest 5-smooth
 * size; butter sections reproduce the analytic Butterworth magnitude.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-par.h"
#include "dyn-sigk.h"
#include "../lib/dyn-xalloc.h"

static int fails, checks;

#define CHECK(c, ...)                                   \
    do {                                                \
        checks++;                                       \
        if (!(c)) {                                     \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static uint64_t rs = 88172645463325252ull;

static double rndu(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (double)(rs >> 11) / 9007199254740992.0 * 2.0 - 1.0;
}

static double dft_err(const double* xr, const double* xi, const double* yr, const double* yi, uint32_t n, int inverse)
{
    double worst = 0.0, peak = 1e-300;
    uint32_t k, t;
    for (k = 0; k < n; k++) {
        long double sr = 0.0L, si = 0.0L;
        for (t = 0; t < n; t++) {
            long double a = (inverse ? 2.0L : -2.0L) * 3.14159265358979323846264338327950288L * (long double)((uint64_t)k * t % n)
                / (long double)n;
            long double c = cosl(a), s = sinl(a);
            sr += (long double)xr[t] * c - (long double)xi[t] * s;
            si += (long double)xr[t] * s + (long double)xi[t] * c;
        }
        if (inverse) {
            sr /= (long double)n;
            si /= (long double)n;
        }
        peak = fmax(peak, (double)fabsl(sr) + (double)fabsl(si));
        worst = fmax(worst, fabs(yr[k] - (double)sr) + fabs(yi[k] - (double)si));
    }
    return worst / peak;
}

static void check_fft(uint32_t n)
{
    double *xr = xmalloc(n * sizeof(double)), *xi = xmalloc(n * sizeof(double));
    double *yr = xmalloc(n * sizeof(double)), *yi = xmalloc(n * sizeof(double));
    uint32_t i;
    int inv;
    for (inv = 0; inv < 2; inv++) {
        double e;
        for (i = 0; i < n; i++) {
            xr[i] = yr[i] = rndu();
            xi[i] = yi[i] = rndu();
        }
        CHECK(dyn_sg_fft(yr, yi, n, inv) == DYN_SG_OK, "fft n=%u", n);
        e = dft_err(xr, xi, yr, yi, n, inv);
        CHECK(e < 1e-13 * (1.0 + log2((double)n)), "%s n=%u relative error %.3g", inv ? "ifft" : "fft", n, e);
    }
    free(xr);
    free(xi);
    free(yr);
    free(yi);
}

static void check_real(uint32_t n)
{
    uint32_t h = n / 2 + 1, i;
    double *x = xmalloc(n * sizeof(double)), *cr = xmalloc(n * sizeof(double)), *ci = calloc(n, sizeof(double));
    double *rr = malloc(h * sizeof(double)), *ri = malloc(h * sizeof(double)), *back = malloc(n * sizeof(double));
    double e = 0.0, eb = 0.0;
    for (i = 0; i < n; i++)
        x[i] = cr[i] = rndu();
    CHECK(dyn_sg_fft(cr, ci, n, 0) == DYN_SG_OK && dyn_sg_rfft(x, n, rr, ri) == DYN_SG_OK, "rfft n=%u", n);
    for (i = 0; i < h; i++)
        e = fmax(e, fabs(rr[i] - cr[i]) + fabs(ri[i] - ci[i]));
    CHECK(e < 1e-12 * sqrt((double)n), "rfft n=%u equals the complex FFT's half (%.3g)", n, e);
    CHECK(ri[0] == 0.0 && (n % 2 || ri[n / 2] == 0.0), "rfft n=%u DC and Nyquist bins are exactly real", n);
    CHECK(dyn_sg_irfft(rr, ri, h, n, back) == DYN_SG_OK, "irfft n=%u", n);
    for (i = 0; i < n; i++)
        eb = fmax(eb, fabs(back[i] - x[i]));
    CHECK(eb < 1e-13 * (1.0 + log2((double)n)), "irfft(rfft(x)) == x for n=%u (%.3g)", n, eb);
    free(x);
    free(cr);
    free(ci);
    free(rr);
    free(ri);
    free(back);
}

typedef struct {
    uint32_t sizes[8];
    double* out[8];
} conc_t;

static void conc_worker(void* arg, unsigned w, unsigned nw, dyn_par_t* par)
{
    conc_t* c = (conc_t*)arg;
    unsigned r, j;
    (void)par;
    for (r = 0; r < 6; r++)
        for (j = w; j < 8; j += nw) {
            uint32_t n = c->sizes[j], i;
            double *re = xmalloc(n * sizeof(double)), *im = xmalloc(n * sizeof(double));
            for (i = 0; i < n; i++) {
                re[i] = sin((double)i * 0.1 + (double)j);
                im[i] = 0.0;
            }
            dyn_sg_fft(re, im, n, 0);
            if (r == 5)
                memcpy(c->out[j], re, n * sizeof(double));
            free(re);
            free(im);
        }
}

static void test_cache_eviction(void)
{
    static const uint32_t sizes[] = { 6, 10, 14, 22, 26, 34, 38, 46, 58, 62, 74, 82, 86, 94, 106, 118, 122, 134, 142, 146 };
    unsigned rep, k;
    for (rep = 0; rep < 3; rep++)
        for (k = 0; k < sizeof sizes / sizeof sizes[0]; k++)
            check_fft(sizes[k]);
}

static void test_concurrent(void)
{
    conc_t c;
    unsigned j, diff = 0;
    static const uint32_t sizes[8] = { 1000, 1009, 4096, 243, 2310, 4099, 31 * 31, 7 * 11 * 13 };
    for (j = 0; j < 8; j++) {
        c.sizes[j] = sizes[j];
        c.out[j] = malloc(sizes[j] * sizeof(double));
    }
    dyn_par_run(8, conc_worker, &c);
    for (j = 0; j < 8; j++) {
        uint32_t n = sizes[j], i;
        double *re = xmalloc(n * sizeof(double)), *im = xmalloc(n * sizeof(double));
        for (i = 0; i < n; i++) {
            re[i] = sin((double)i * 0.1 + (double)j);
            im[i] = 0.0;
        }
        dyn_sg_fft(re, im, n, 0);
        for (i = 0; i < n; i++)
            diff += re[i] != c.out[j][i];
        free(re);
        free(im);
        free(c.out[j]);
    }
    CHECK(diff == 0, "FFTs run concurrently on shared cached plans equal the serial ones (%u differ)", diff);
}

static void test_welch_parallel(void)
{
    uint32_t n = 400000, i, nb;
    double *x = malloc(n * sizeof(double)), *p1, *p2;
    dyn_sg_welch_t o;
    memset(&o, 0, sizeof o);
    o.fs = 1000.0;
    o.window = DYN_SG_WIN_HANN;
    o.nperseg = 256;
    o.noverlap = 128;
    o.nfft = 256;
    o.detrend = 1;
    o.density = 1;
    nb = o.nfft / 2 + 1;
    p1 = malloc(nb * sizeof(double));
    p2 = malloc(nb * sizeof(double));
    for (i = 0; i < n; i++)
        x[i] = sin(2.0 * 3.141592653589793 * 50.0 * (double)i / 1000.0) + 0.3 * rndu();
    dyn_par_set_spawn_limit(0);
    CHECK(dyn_sg_welch(x, n, &o, p1) == DYN_SG_OK, "welch serial");
    dyn_par_set_spawn_limit(-1);
    CHECK(dyn_sg_welch(x, n, &o, p2) == DYN_SG_OK, "welch parallel");
    CHECK(memcmp(p1, p2, nb * sizeof(double)) == 0, "welch is bit-identical on one thread and many");
    {
        uint32_t best = 0;
        for (i = 1; i < nb; i++)
            if (p1[i] > p1[best])
                best = i;
        CHECK(fabs((double)best * 1000.0 / 256.0 - 50.0) < 4.0, "welch peak at 50 Hz (bin %u)", best);
    }
    free(x);
    free(p1);
    free(p2);
}

static void conv_check(uint32_t na, uint32_t nb)
{
    uint32_t L = na + nb - 1, i, j;
    double *a = xmalloc(na * sizeof(double)), *b = xmalloc(nb * sizeof(double)), *f = malloc(L * sizeof(double));
    double e = 0.0, peak = 0.0;
    for (i = 0; i < na; i++)
        a[i] = rndu();
    for (i = 0; i < nb; i++)
        b[i] = rndu();
    CHECK(dyn_sg_convolve(a, na, b, nb, DYN_SG_FULL, f) == DYN_SG_OK, "convolve fft path");
    for (i = 0; i < L; i += (i < 8 || i + 8 > L) ? 1 : 37) {
        double s = 0.0;
        for (j = 0; j < nb; j++)
            if (i >= j && i - j < na)
                s += a[i - j] * b[j];
        e = fmax(e, fabs(s - f[i]));
        peak = fmax(peak, fabs(s));
    }
    CHECK(e < 1e-12 * peak * 30.0, "FFT convolution %ux%u equals the direct sum (%.3g of %.3g)", na, nb, e, peak);
    free(a);
    free(b);
    free(f);
}

static void test_convolve_paths(void)
{
    conv_check(3001, 777);
    conv_check(1025, 1025);
    conv_check(1537, 1536);
}

static int smooth5(uint32_t n)
{
    while (n % 2 == 0)
        n /= 2;
    while (n % 3 == 0)
        n /= 3;
    while (n % 5 == 0)
        n /= 5;
    return n == 1;
}

static void test_next_fast(void)
{
    uint32_t n, bad = 0;
    for (n = 1; n < 5000; n++) {
        uint32_t m = dyn_sg_next_fast(n), k;
        if (m < n || !smooth5(m))
            bad++;
        for (k = n; k < m; k++)
            if (smooth5(k))
                bad++;
    }
    CHECK(bad == 0, "next_fast is the smallest 5-smooth size >= n (%u bad)", bad);
}

static void test_butter_magnitude(void)
{
    double sos[6 * 40], w[64], hr[64], hi[64];
    uint32_t ns, k, N;
    double e = 0.0;
    for (N = 1; N <= 12; N++) {
        double wc = 0.3;
        CHECK(dyn_sg_butter(N, wc, 0.0, DYN_SG_LOWPASS, sos, &ns) == DYN_SG_OK && ns == (N + 1) / 2, "butter N=%u", N);
        for (k = 0; k < 64; k++)
            w[k] = 3.141592653589793 * ((double)k + 0.5) / 64.0;
        dyn_sg_sosfreqz(sos, ns, w, 64, hr, hi);
        for (k = 0; k < 64; k++) {
            double ratio = tan(w[k] / 2.0) / tan(3.141592653589793 * wc / 2.0);
            double want = 1.0 / sqrt(1.0 + pow(ratio, 2.0 * (double)N));
            e = fmax(e, fabs(hypot(hr[k], hi[k]) - want));
        }
    }
    CHECK(e < 1e-10, "butter lowpass magnitude is 1/sqrt(1+(tan(w/2)/tan(wc/2))^2N) (%.3g)", e);
}

int main(void)
{
    uint32_t n;
    static const uint32_t big[] = { 192, 210, 243, 256, 289, 343, 361, 512, 625, 729, 841, 961, 1009, 1024, 1155, 2048,
        2310, 3072, 4099, 6561, 7919 };
    for (n = 1; n <= 160; n++)
        check_fft(n);
    for (n = 0; n < sizeof big / sizeof big[0]; n++)
        check_fft(big[n]);
    for (n = 1; n <= 70; n++)
        check_real(n);
    check_real(1009);
    check_real(4096);
    check_real(65536);
    check_real(2 * 4099);
    test_cache_eviction();
    test_concurrent();
    test_welch_parallel();
    test_convolve_paths();
    test_next_fast();
    test_butter_magnitude();
    printf("test_sigk_core: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
