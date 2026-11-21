#include "sigk-int.h"
#include "../dyn-par.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

uint32_t dyn_sg_conv_len(uint32_t na, uint32_t nb, dyn_sg_mode_t mode)
{
    if (na == 0 || nb == 0)
        return 0;
    if (mode == DYN_SG_SAME)
        return na;
    if (mode == DYN_SG_VALID)
        return (na > nb ? na - nb : nb - na) + 1;
    return na + nb - 1;
}

static int sg_conv_full(const double* a, uint32_t na, const double* b, uint32_t nb, double* out)
{
    uint32_t L = na + nb - 1, i, j;
    if ((uint64_t)na * nb <= 65536u || (na < 48 || nb < 48)) {
        memset(out, 0, (size_t)L * sizeof(double));
        for (i = 0; i < na; i++) {
            double ai = a[i];
            for (j = 0; j < nb; j++)
                out[i + j] += ai * b[j];
        }
        return DYN_SG_OK;
    }
    {
        uint32_t nf = 2 * dyn_sg_next_fast((L + 1) / 2), h = nf / 2 + 1;
        double *pa, *pb, *ar, *ai, *br, *bi;
        int rc;
        if (nf > DYN_SG_MAX_N)
            return DYN_SG_E_SIZE;
        pa = (double*)calloc((size_t)2 * nf + (size_t)4 * h, sizeof(double));
        if (!pa)
            return DYN_SG_E_OOM;
        pb = pa + nf;
        ar = pb + nf;
        ai = ar + h;
        br = ai + h;
        bi = br + h;
        memcpy(pa, a, (size_t)na * sizeof(double));
        memcpy(pb, b, (size_t)nb * sizeof(double));
        rc = dyn_sg_rfft(pa, nf, ar, ai);
        if (rc == DYN_SG_OK)
            rc = dyn_sg_rfft(pb, nf, br, bi);
        if (rc == DYN_SG_OK) {
            for (i = 0; i < h; i++) {
                double r = ar[i] * br[i] - ai[i] * bi[i], m = ar[i] * bi[i] + ai[i] * br[i];
                ar[i] = r;
                ai[i] = m;
            }
            rc = dyn_sg_irfft(ar, ai, h, nf, pa);
        }
        if (rc == DYN_SG_OK)
            memcpy(out, pa, (size_t)L * sizeof(double));
        free(pa);
        return rc;
    }
}

int dyn_sg_convolve(const double* a, uint32_t na, const double* b, uint32_t nb, dyn_sg_mode_t mode, double* out)
{
    uint32_t L, len, start;
    double* full;
    int rc;
    if (na == 0 || nb == 0)
        return DYN_SG_E_SIZE;
    L = na + nb - 1;
    if (mode == DYN_SG_FULL)
        return sg_conv_full(a, na, b, nb, out);
    full = (double*)malloc((size_t)L * sizeof(double));
    if (!full)
        return DYN_SG_E_OOM;
    rc = sg_conv_full(a, na, b, nb, full);
    len = dyn_sg_conv_len(na, nb, mode);
    start = (L - len) / 2;
    if (rc == DYN_SG_OK)
        memcpy(out, full + start, (size_t)len * sizeof(double));
    free(full);
    return rc;
}

#define SG_WELCH_GROUP 16u

typedef struct {
    const double* x;
    const dyn_sg_welch_t* o;
    const double* win;
    double scale;
    uint32_t nseg, step, ngroups, nbins;
    double* groups;
    atomic_uint next;
    atomic_int failed;
} sg_welch_job_t;

static void sg_welch_worker(void* arg, unsigned w, unsigned nw, dyn_par_t* par)
{
    sg_welch_job_t* j = (sg_welch_job_t*)arg;
    const dyn_sg_welch_t* o = j->o;
    uint32_t nfft = o->nfft, nps = o->nperseg, g, s, k;
    double* buf = (double*)malloc(((size_t)nfft + 2u * j->nbins) * sizeof(double));
    double *re, *im;
    (void)w;
    (void)nw;
    (void)par;
    if (!buf) {
        atomic_store(&j->failed, 1);
        return;
    }
    re = buf + nfft;
    im = re + j->nbins;
    while ((g = atomic_fetch_add(&j->next, 1)) < j->ngroups) {
        double* acc = j->groups + (size_t)g * j->nbins;
        uint32_t s0 = g * SG_WELCH_GROUP, s1 = s0 + SG_WELCH_GROUP < j->nseg ? s0 + SG_WELCH_GROUP : j->nseg;
        memset(acc, 0, (size_t)j->nbins * sizeof(double));
        for (s = s0; s < s1; s++) {
            const double* seg = j->x + (size_t)s * j->step;
            double mean = 0.0;
            if (o->detrend) {
                for (k = 0; k < nps; k++)
                    mean += seg[k];
                mean /= (double)nps;
            }
            for (k = 0; k < nps; k++)
                buf[k] = (seg[k] - mean) * j->win[k];
            for (; k < nfft; k++)
                buf[k] = 0.0;
            if (dyn_sg_rfft(buf, nfft, re, im) != DYN_SG_OK) {
                atomic_store(&j->failed, 1);
                free(buf);
                return;
            }
            for (k = 0; k < j->nbins; k++)
                acc[k] += re[k] * re[k] + im[k] * im[k];
        }
    }
    free(buf);
}

int dyn_sg_welch(const double* x, uint32_t n, const dyn_sg_welch_t* o, double* pxx)
{
    sg_welch_job_t j;
    uint32_t nps = o->nperseg, k, g;
    double *win, s1 = 0.0, s2 = 0.0;
    unsigned nw;
    if (nps == 0 || nps > n || o->noverlap >= nps || o->nfft < nps || o->nfft > DYN_SG_MAX_N)
        return DYN_SG_E_SIZE;
    if (!(o->fs > 0.0))
        return DYN_SG_E_DOMAIN;
    memset(&j, 0, sizeof j);
    j.x = x;
    j.o = o;
    j.step = nps - o->noverlap;
    j.nseg = (n - o->noverlap) / j.step;
    j.nbins = o->nfft / 2 + 1;
    j.ngroups = (j.nseg + SG_WELCH_GROUP - 1) / SG_WELCH_GROUP;
    win = (double*)malloc((size_t)nps * sizeof(double));
    j.groups = (double*)malloc((size_t)j.ngroups * j.nbins * sizeof(double));
    if (!win || !j.groups) {
        free(win);
        free(j.groups);
        return DYN_SG_E_OOM;
    }
    dyn_sg_window(o->window, o->beta, nps, 0, win);
    for (k = 0; k < nps; k++) {
        s1 += win[k];
        s2 += win[k] * win[k];
    }
    j.win = win;
    atomic_init(&j.next, 0);
    atomic_init(&j.failed, 0);
    nw = dyn_par_plan((size_t)j.nseg * o->nfft, 1u << 15, 0);
    if (nw > j.ngroups)
        nw = j.ngroups;
    dyn_par_run(nw ? nw : 1, sg_welch_worker, &j);
    if (atomic_load(&j.failed)) {
        free(win);
        free(j.groups);
        return DYN_SG_E_OOM;
    }
    j.scale = o->density ? 1.0 / (o->fs * s2) : 1.0 / (s1 * s1);
    for (k = 0; k < j.nbins; k++) {
        double t = 0.0;
        for (g = 0; g < j.ngroups; g++)
            t += j.groups[(size_t)g * j.nbins + k];
        t = t * j.scale / (double)j.nseg;
        if (k > 0 && !((o->nfft & 1u) == 0 && k == j.nbins - 1))
            t *= 2.0;
        pxx[k] = t;
    }
    free(win);
    free(j.groups);
    return DYN_SG_OK;
}

int dyn_sg_hilbert(const double* x, uint32_t nx, uint32_t n, double* re, double* im)
{
    uint32_t k, h = n / 2 + 1;
    double* pad;
    int rc;
    if (n == 0 || n > DYN_SG_MAX_N)
        return DYN_SG_E_SIZE;
    pad = (double*)calloc(n, sizeof(double));
    if (!pad)
        return DYN_SG_E_OOM;
    memcpy(pad, x, (size_t)(nx < n ? nx : n) * sizeof(double));
    rc = dyn_sg_rfft(pad, n, re, im);
    free(pad);
    if (rc != DYN_SG_OK)
        return rc;
    for (k = 1; k < h; k++) {
        double g = (n % 2 == 0 && k == n / 2) ? 1.0 : 2.0;
        re[k] *= g;
        im[k] *= g;
    }
    for (k = h; k < n; k++) {
        re[k] = 0.0;
        im[k] = 0.0;
    }
    return dyn_sg_fft(re, im, n, 1);
}

void dyn_sg_goertzel(const double* x, uint32_t n, double w, double* re, double* im)
{
    double c = cos(w), coeff = 2.0 * c, s1 = 0.0, s2 = 0.0, yr, yi, cr, ci;
    uint32_t i;
    for (i = 0; i < n; i++) {
        double s = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    yr = s1 - c * s2;
    yi = sin(w) * s2;
    if (n == 0) {
        *re = 0.0;
        *im = 0.0;
        return;
    }
    cr = cos(w * (double)(n - 1));
    ci = -sin(w * (double)(n - 1));
    *re = yr * cr - yi * ci;
    *im = yr * ci + yi * cr;
}
