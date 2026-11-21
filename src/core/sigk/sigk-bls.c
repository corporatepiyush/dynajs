#include "sigk-int.h"
#include "../dyn-par.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const double* t;
    const double* yc;
    uint32_t n;
    const double* periods;
    uint32_t np;
    const double* durs;
    uint32_t nd;
    uint32_t nbins, min_points;
    double *power, *dur, *depth, *epoch;
    atomic_uint next;
} sg_bls_job_t;

static double sg_fmod(double x, double y)
{
    double q, n, r;
    if (!(y > 0.0) || !isfinite(x) || !isfinite(y))
        return fmod(x, y);
    q = x / y;
    if (!(fabs(q) < 4503599627370496.0))
        return fmod(x, y);
    n = trunc(q);
    r = fma(-n, y, x);
    if (x >= 0.0) {
        if (r < 0.0)
            r = fma(-(n - 1.0), y, x);
        else if (r >= y)
            r = fma(-(n + 1.0), y, x);
    } else {
        if (r > 0.0)
            r = fma(-(n + 1.0), y, x);
        else if (r <= -y)
            r = fma(-(n - 1.0), y, x);
    }
    return r == 0.0 ? copysign(0.0, x) : r;
}

static void sg_bls_one(const sg_bls_job_t* j, uint32_t k, double* bs, double* bn, double* cs, double* cn)
{
    uint32_t nb = j->nbins, i, d, s;
    double P = j->periods[k], N = (double)j->n, best = 0.0, bdur = DYN_NAN, bdep = DYN_NAN, bep = DYN_NAN;
    memset(bs, 0, (size_t)nb * sizeof(double));
    memset(bn, 0, (size_t)nb * sizeof(double));
    for (i = 0; i < j->n; i++) {
        double ph = sg_fmod(j->t[i], P), b;
        uint32_t bi;
        if (ph < 0.0)
            ph += P;
        b = floor(ph / P * (double)nb);
        bi = b >= (double)nb ? nb - 1 : (uint32_t)b;
        bs[bi] += j->yc[i];
        bn[bi] += 1.0;
    }
    cs[0] = 0.0;
    cn[0] = 0.0;
    for (s = 0; s < 2 * nb; s++) {
        uint32_t m = s < nb ? s : s - nb;
        cs[s + 1] = cs[s] + bs[m];
        cn[s + 1] = cn[s] + bn[m];
    }
    for (d = 0; d < j->nd; d++) {
        double wd = round(j->durs[d] / P * (double)nb);
        uint32_t w;
        if (wd < 1.0)
            wd = 1.0;
        if (wd >= (double)nb)
            continue;
        w = (uint32_t)wd;
        for (s = 0; s < nb; s++) {
            double sr = cs[s + w] - cs[s], cnt = cn[s + w] - cn[s], r, pw;
            if (cnt < (double)j->min_points || cnt >= N)
                continue;
            r = cnt / N;
            pw = sr * sr / (N * r * (1.0 - r));
            if (pw > best) {
                best = pw;
                bdur = j->durs[d];
                bdep = -sr / cnt;
                bep = ((double)s + (double)w / 2.0) / (double)nb * P;
                if (bep >= P)
                    bep -= P;
            }
        }
    }
    j->power[k] = best;
    j->dur[k] = bdur;
    j->depth[k] = bdep;
    j->epoch[k] = bep;
}

static void sg_bls_worker(void* arg, unsigned w, unsigned nw, dyn_par_t* par)
{
    sg_bls_job_t* j = (sg_bls_job_t*)arg;
    size_t nb = j->nbins;
    double* buf = (double*)malloc((2u * nb + 2u * (2u * nb + 1u)) * sizeof(double));
    uint32_t k;
    (void)w;
    (void)nw;
    (void)par;
    if (!buf)
        return;
    while ((k = atomic_fetch_add(&j->next, 1)) < j->np)
        sg_bls_one(j, k, buf, buf + nb, buf + 2 * nb, buf + 2 * nb + (2 * nb + 1));
    free(buf);
}

int dyn_sg_bls(const double* t, const double* y, uint32_t n, const double* periods, uint32_t np, const double* durs,
    uint32_t nd, const dyn_sg_bls_t* o, double* power, double* dur, double* depth, double* epoch)
{
    sg_bls_job_t j;
    double mean = 0.0, *yc;
    uint32_t i;
    unsigned nw;
    if (n == 0 || np == 0 || nd == 0 || o->nbins < 2 || o->nbins > DYN_SG_BLS_MAX_BINS)
        return DYN_SG_E_SIZE;
    yc = (double*)malloc((size_t)n * sizeof(double));
    if (!yc)
        return DYN_SG_E_OOM;
    for (i = 0; i < n; i++)
        mean += y[i];
    mean /= (double)n;
    for (i = 0; i < n; i++)
        yc[i] = y[i] - mean;
    memset(&j, 0, sizeof j);
    j.t = t;
    j.yc = yc;
    j.n = n;
    j.periods = periods;
    j.np = np;
    j.durs = durs;
    j.nd = nd;
    j.nbins = o->nbins;
    j.min_points = o->min_points;
    j.power = power;
    j.dur = dur;
    j.depth = depth;
    j.epoch = epoch;
    atomic_init(&j.next, 0);
    nw = dyn_par_plan((size_t)np * ((size_t)n + (size_t)o->nbins * nd), 1u << 16, 0);
    if (nw > np)
        nw = np;
    dyn_par_run(nw ? nw : 1, sg_bls_worker, &j);
    free(yc);
    return atomic_load(&j.next) < np ? DYN_SG_E_OOM : DYN_SG_OK;
}
