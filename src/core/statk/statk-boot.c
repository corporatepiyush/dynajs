#include "statk-int.h"
#include "../dyn-par.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define ST_GOLDEN 0x9E3779B97F4A7C15ull

uint64_t dyn_st_boot_draw(uint64_t* state)
{
    uint64_t z = (*state += ST_GOLDEN);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

typedef struct {
    const dyn_st_map_t* m;
    double* v;
    uint32_t n;
} st_map_job_t;

static void st_map_range(const dyn_st_map_t* m, double* v, uint32_t lo, uint32_t hi)
{
    uint32_t i;
    if (m->f1)
        for (i = lo; i < hi; i++)
            v[i] = m->f1(v[i]);
    else if (m->f2)
        for (i = lo; i < hi; i++)
            v[i] = m->f2(v[i], m->p0);
    else
        for (i = lo; i < hi; i++)
            v[i] = m->f3(v[i], m->p0, m->p1);
}

static void st_map_worker(void* arg, unsigned w, unsigned nw, dyn_par_t* par)
{
    st_map_job_t* j = (st_map_job_t*)arg;
    (void)par;
    st_map_range(j->m, j->v, (uint32_t)(((uint64_t)j->n * w) / nw), (uint32_t)(((uint64_t)j->n * (w + 1)) / nw));
}

void dyn_st_map(const dyn_st_map_t* m, double* v, uint32_t n)
{
    st_map_job_t j;
    unsigned nw = dyn_par_plan(n, 4096, 0);
    if (nw <= 1) {
        st_map_range(m, v, 0, n);
        return;
    }
    j.m = m;
    j.v = v;
    j.n = n;
    dyn_par_run(nw, st_map_worker, &j);
}

static inline uint32_t st_bounded(uint64_t r, uint32_t n)
{
    return (uint32_t)(((unsigned __int128)r * n) >> 64);
}

static double st_select(double* v, uint32_t n, uint32_t kk)
{
    int64_t l = 0, m = (int64_t)n - 1, k = (int64_t)kk;
    while (l < m) {
        double x = v[k];
        int64_t i = l, j = m;
        do {
            while (v[i] < x)
                i++;
            while (x < v[j])
                j--;
            if (i <= j) {
                double t = v[i];
                v[i] = v[j];
                v[j] = t;
                i++;
                j--;
            }
        } while (i <= j);
        if (j < k)
            l = i;
        if (k < i)
            m = j;
    }
    return v[k];
}

static double st_stat(double* v, uint32_t n, dyn_st_boot_stat_t stat)
{
    if (stat == DYN_ST_BOOT_MEDIAN) {
        double hi = st_select(v, n, n / 2), lo;
        uint32_t i;
        if (n & 1)
            return hi;
        lo = v[0];
        for (i = 1; i < n / 2; i++)
            if (v[i] > lo)
                lo = v[i];
        return 0.5 * (lo + hi);
    } else {
        double s = 0.0;
        uint32_t i;
        for (i = 0; i < n; i++)
            s += v[i];
        return s / (double)n;
    }
}

static double st_combine(double sa, double sb, int two, dyn_st_boot_cmp_t cmp)
{
    if (!two)
        return sa;
    return cmp == DYN_ST_BOOT_RATIO ? sa / sb : sa - sb;
}

typedef struct {
    const double *a, *b;
    uint32_t na, nb;
    dyn_st_boot_t* o;
    double* out;
    atomic_int failed, stopped;
} st_boot_job_t;

static void st_boot_worker(void* arg, unsigned w, unsigned nw, dyn_par_t* par)
{
    st_boot_job_t* j = (st_boot_job_t*)arg;
    uint32_t R = j->o->resamples, lo = (uint32_t)(((uint64_t)R * w) / nw), hi = (uint32_t)(((uint64_t)R * (w + 1)) / nw);
    uint32_t r, i, cap = j->na > j->nb ? j->na : j->nb;
    double* buf;
    (void)par;
    buf = (double*)malloc((size_t)(cap ? cap : 1) * sizeof(double));
    if (!buf) {
        atomic_store(&j->failed, 1);
        return;
    }
    for (r = lo; r < hi; r++) {
        uint64_t t = j->o->seed + ST_GOLDEN * (uint64_t)r, s = dyn_st_boot_draw(&t);
        double sa, sb = 0.0;
        if (((r - lo) & 63u) == 0 && j->o->stop && j->o->stop(j->o->ud)) {
            atomic_store(&j->stopped, 1);
            break;
        }
        if (atomic_load_explicit(&j->stopped, memory_order_relaxed))
            break;
        for (i = 0; i < j->na; i++)
            buf[i] = j->a[st_bounded(dyn_st_boot_draw(&s), j->na)];
        sa = st_stat(buf, j->na, j->o->stat);
        if (j->b) {
            for (i = 0; i < j->nb; i++)
                buf[i] = j->b[st_bounded(dyn_st_boot_draw(&s), j->nb)];
            sb = st_stat(buf, j->nb, j->o->stat);
        }
        j->out[r] = st_combine(sa, sb, j->b != NULL, j->o->cmp);
    }
    free(buf);
}

static double st_percentile(const double* sorted, uint32_t n, double q)
{
    double pos = q * (double)(n - 1), f = floor(pos);
    uint32_t k = (uint32_t)f;
    if (k + 1 >= n)
        return sorted[n - 1];
    return sorted[k] + (pos - f) * (sorted[k + 1] - sorted[k]);
}

int dyn_st_bootstrap(const double* a, uint32_t na, const double* b, uint32_t nb, dyn_st_boot_t* o)
{
    st_boot_job_t j;
    double *out, *sorted, *tmp;
    uint32_t cap = na > nb ? na : nb;
    unsigned nw;
    int rc;
    if (na == 0 || (b && nb == 0) || o->resamples < 2)
        return DYN_ST_E_SIZE;
    if (!(o->level > 0.0 && o->level < 1.0))
        return DYN_ST_E_DOMAIN;
    if (dyn_st_has_nan(a, na) || (b && dyn_st_has_nan(b, nb)))
        return DYN_ST_E_NAN;
    out = (double*)malloc(((size_t)o->resamples * 2 + cap) * sizeof(double));
    if (!out)
        return DYN_ST_E_OOM;
    sorted = out + o->resamples;
    tmp = sorted + o->resamples;
    memcpy(tmp, a, (size_t)na * sizeof(double));
    {
        double sa = st_stat(tmp, na, o->stat), sb = 0.0;
        if (b) {
            memcpy(tmp, b, (size_t)nb * sizeof(double));
            sb = st_stat(tmp, nb, o->stat);
        }
        o->estimate = st_combine(sa, sb, b != NULL, o->cmp);
    }
    j.a = a;
    j.b = b;
    j.na = na;
    j.nb = b ? nb : 0;
    j.o = o;
    j.out = out;
    atomic_init(&j.failed, 0);
    atomic_init(&j.stopped, 0);
    nw = dyn_par_plan((size_t)o->resamples * ((size_t)na + j.nb), 1u << 16, 0);
    if (nw > o->resamples)
        nw = o->resamples;
    dyn_par_run(nw ? nw : 1, st_boot_worker, &j);
    if (atomic_load(&j.failed)) {
        free(out);
        return DYN_ST_E_OOM;
    }
    if (atomic_load(&j.stopped)) {
        free(out);
        return DYN_ST_E_CANCELLED;
    }
    if (dyn_st_has_nan(out, o->resamples)) {
        o->low = DYN_NAN;
        o->high = DYN_NAN;
        free(out);
        return DYN_ST_OK;
    }
    rc = dyn_st_sort_copy(out, o->resamples, sorted);
    if (rc == DYN_ST_OK) {
        o->low = st_percentile(sorted, o->resamples, (1.0 - o->level) / 2.0);
        o->high = st_percentile(sorted, o->resamples, (1.0 + o->level) / 2.0);
    }
    free(out);
    return rc;
}
