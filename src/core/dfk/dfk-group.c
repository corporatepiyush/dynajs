#include "dfk.h"
#include "../dyn-par.h"

#define DFK_BLOCK_ROWS (1u << 20)
#define DFK_BLOCK_MAX_GROUPS (1u << 16)

double df_get_call(const void* p, DFType t, uint32_t i)
{
    return df_get(p, t, i);
}

#define DFK_RANGE(CTYPE)          \
    do {                          \
        const CTYPE* x = p;       \
        CTYPE a = 0, z = 0;       \
        for (i = 0; i < n; i++) { \
            CTYPE v = x[i];       \
            if (v < a)            \
                a = v;            \
            if (v > z)            \
                z = v;            \
        }                         \
        if ((double)a < 0.0)      \
            neg = 1;              \
        hi = (double)z;           \
    } while (0)

typedef struct {
    const int32_t* x;
    uint32_t n, nblocks;
    int32_t lo[64], hi[64];
} dfk_range_job;

static void dfk_range_worker(void* arg, unsigned w, unsigned nw, dyn_par_t* par)
{
    dfk_range_job* j = (dfk_range_job*)arg;
    uint32_t b, i;
    (void)par;
    for (b = w; b < j->nblocks; b += nw) {
        uint32_t s = b * DFK_BLOCK_ROWS, e = s + DFK_BLOCK_ROWS < j->n ? s + DFK_BLOCK_ROWS : j->n;
        int32_t a = 0, z = 0;
        for (i = s; i < e; i++) {
            int32_t v = j->x[i];
            a = v < a ? v : a;
            z = v > z ? v : z;
        }
        j->lo[b] = a;
        j->hi[b] = z;
    }
}

static int dfk_range_i32_par(const int32_t* x, uint32_t n, int32_t* plo, int32_t* phi)
{
    dfk_range_job j;
    uint32_t b;
    j.x = x;
    j.n = n;
    j.nblocks = (n + DFK_BLOCK_ROWS - 1) / DFK_BLOCK_ROWS;
    if (j.nblocks > 64)
        return -1;
    dyn_par_run(dyn_par_plan(j.nblocks, 1, 0), dfk_range_worker, &j);
    *plo = 0;
    *phi = 0;
    for (b = 0; b < j.nblocks; b++) {
        *plo = j.lo[b] < *plo ? j.lo[b] : *plo;
        *phi = j.hi[b] > *phi ? j.hi[b] : *phi;
    }
    return 0;
}

int dfk_group_range(DFType t, const void* p, uint32_t n, uint32_t* pnkeys, uint32_t* pngroups)
{
    double hi = 0.0;
    int neg = 0;
    uint32_t i, ngroups;
    if (t == DF_F64 || t == DF_F32)
        return DFK_E_FLOAT_KEY;
    switch ((int)t) {
    case DF_I32:
    case DF_STR: {
        int32_t rlo, rhi;
        if (n >= 2 * DFK_BLOCK_ROWS && dfk_range_i32_par((const int32_t*)p, n, &rlo, &rhi) == 0) {
            neg = rlo < 0;
            hi = (double)rhi;
            break;
        }
        DFK_RANGE(int32_t);
        break;
    }
    case DF_U32:
        DFK_RANGE(uint32_t);
        break;
    case DF_I16:
        DFK_RANGE(int16_t);
        break;
    case DF_U16:
        DFK_RANGE(uint16_t);
        break;
    case DF_I8:
        DFK_RANGE(int8_t);
        break;
    default:
        DFK_RANGE(uint8_t);
        break;
    }
    if (neg)
        return DFK_E_NEG_KEY;
    if (hi + 1 > (double)DF_MAX_GROUPS)
        return DFK_E_TOO_MANY;
    ngroups = n ? (uint32_t)hi + 1 : 0;
    *pnkeys = ngroups;
    *pngroups = ngroups ? ngroups : 1;
    return DFK_OK;
}

#undef DFK_RANGE

#define DFK_KEYS_ONE(CTYPE)                \
    do {                                   \
        const CTYPE* x = p;                \
        for (i = 0; i < n; i++)            \
            k[i] = (uint32_t)(double)x[i]; \
    } while (0)

uint32_t* dfk_keys_u32(DFType t, const void* p, uint32_t n)
{
    uint32_t *k, i;
    if (!n)
        return NULL;
    k = (uint32_t*)malloc((size_t)n * sizeof(*k));
    if (!k)
        return NULL;
    switch ((int)t) {
    case DF_I32:
    case DF_STR:
        DFK_KEYS_ONE(int32_t);
        break;
    case DF_U32:
        DFK_KEYS_ONE(uint32_t);
        break;
    case DF_I16:
        DFK_KEYS_ONE(int16_t);
        break;
    case DF_U16:
        DFK_KEYS_ONE(uint16_t);
        break;
    case DF_I8:
        DFK_KEYS_ONE(int8_t);
        break;
    case DF_U8:
        DFK_KEYS_ONE(uint8_t);
        break;
    default:
        DFK_KEYS_ONE(double);
        break;
    }
    return k;
}

#undef DFK_KEYS_ONE

static inline __attribute__((always_inline)) void dfk_scan(DFType kt, const void* kp, DFType vt,
    const void* vp, uint32_t n, const uint8_t* mask, uint32_t ngroups, const uint32_t* gk,
    unsigned aggs, int f64, const dfk_acc_t* a)
{
    const int want_v = (aggs & (DFK_AGG_SUM | DFK_AGG_MEAN | DFK_AGG_MIN | DFK_AGG_MAX)) != 0;
    const int want_c = (aggs & (DFK_AGG_COUNT | DFK_AGG_MEAN)) != 0;
    const int want_s = (aggs & (DFK_AGG_SUM | DFK_AGG_MEAN)) != 0;
    const double* xf = (const double*)vp;
    double *sum = a->sum, *mn = a->min, *mx = a->max;
    uint32_t* cnt = a->cnt;
    uint32_t i, g;
    for (i = 0; i < n; i++) {
        double v = 0;
        if (mask && !mask[i])
            continue;
        g = gk ? gk[i] : (uint32_t)df_get(kp, kt, i);
        if (g >= ngroups)
            continue;
        if (want_v)
            v = f64 ? xf[i] : df_get(vp, vt, i);
        if (want_s)
            sum[g] += v;
        if (aggs & DFK_AGG_MIN)
            if (v < mn[g] || mn[g] != mn[g])
                mn[g] = v;
        if (aggs & DFK_AGG_MAX)
            if (v > mx[g] || mx[g] != mx[g])
                mx[g] = v;
        if (want_c)
            cnt[g]++;
    }
}

static inline __attribute__((always_inline)) void dfk_scan_i32(const int32_t* kp, const double* xf, uint32_t n,
    const uint8_t* mask, uint32_t ngroups, unsigned aggs, const dfk_acc_t* a)
{
    const int want_c = (aggs & (DFK_AGG_COUNT | DFK_AGG_MEAN)) != 0;
    const int want_s = (aggs & (DFK_AGG_SUM | DFK_AGG_MEAN)) != 0;
    double *sum = a->sum, *mn = a->min, *mx = a->max;
    uint32_t* cnt = a->cnt;
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint32_t g = (uint32_t)kp[i];
        double v;
        if (mask && !mask[i])
            continue;
        if (g >= ngroups)
            continue;
        v = xf ? xf[i] : 0.0;
        if (want_s)
            sum[g] += v;
        if (aggs & DFK_AGG_MIN)
            if (v < mn[g] || mn[g] != mn[g])
                mn[g] = v;
        if (aggs & DFK_AGG_MAX)
            if (v > mx[g] || mx[g] != mx[g])
                mx[g] = v;
        if (want_c)
            cnt[g]++;
    }
}

static void dfk_scan_i32_any(const int32_t* kp, const double* xf, uint32_t n, const uint8_t* mask,
    uint32_t ngroups, unsigned aggs, const dfk_acc_t* a);

typedef struct {
    const int32_t* kp;
    const double* xf;
    const uint8_t* mask;
    uint32_t n, ngroups, nblocks;
    unsigned aggs;
    dfk_acc_t* acc;
} dfk_blk_job;

static void dfk_acc_init(const dfk_acc_t* a, uint32_t ngroups, unsigned aggs)
{
    uint32_t i;
    if (aggs & (DFK_AGG_SUM | DFK_AGG_MEAN))
        memset(a->sum, 0, (size_t)ngroups * sizeof(double));
    if (aggs & DFK_AGG_MIN)
        for (i = 0; i < ngroups; i++)
            a->min[i] = (double)NAN;
    if (aggs & DFK_AGG_MAX)
        for (i = 0; i < ngroups; i++)
            a->max[i] = (double)NAN;
    if (aggs & (DFK_AGG_COUNT | DFK_AGG_MEAN))
        memset(a->cnt, 0, (size_t)ngroups * sizeof(uint32_t));
}

static void dfk_blk_worker(void* arg, unsigned w, unsigned nw, dyn_par_t* par)
{
    dfk_blk_job* j = (dfk_blk_job*)arg;
    uint32_t b;
    (void)par;
    for (b = w; b < j->nblocks; b += nw) {
        uint32_t lo = b * DFK_BLOCK_ROWS, hi = lo + DFK_BLOCK_ROWS < j->n ? lo + DFK_BLOCK_ROWS : j->n;
        if (b)
            dfk_acc_init(&j->acc[b], j->ngroups, j->aggs);
        dfk_scan_i32_any(j->kp + lo, j->xf ? j->xf + lo : NULL, hi - lo, j->mask ? j->mask + lo : NULL, j->ngroups, j->aggs, &j->acc[b]);
    }
}

static int dfk_group_scan_blocked(const int32_t* kp, const double* xf, uint32_t n, const uint8_t* mask,
    uint32_t ngroups, unsigned aggs, const dfk_acc_t* a)
{
    dfk_blk_job j;
    uint32_t nb = (n + DFK_BLOCK_ROWS - 1) / DFK_BLOCK_ROWS, b, g;
    size_t per = (size_t)ngroups;
    double* fbuf = NULL;
    uint32_t* cbuf = NULL;
    int nf = ((aggs & (DFK_AGG_SUM | DFK_AGG_MEAN)) ? 1 : 0) + ((aggs & DFK_AGG_MIN) ? 1 : 0) + ((aggs & DFK_AGG_MAX) ? 1 : 0);
    j.acc = (dfk_acc_t*)calloc(nb, sizeof(dfk_acc_t));
    if (nf)
        fbuf = (double*)malloc(per * (size_t)nf * (nb - 1) * sizeof(double));
    if (aggs & (DFK_AGG_COUNT | DFK_AGG_MEAN))
        cbuf = (uint32_t*)malloc(per * (nb - 1) * sizeof(uint32_t));
    if (!j.acc || (nf && !fbuf) || ((aggs & (DFK_AGG_COUNT | DFK_AGG_MEAN)) && !cbuf)) {
        free(j.acc);
        free(fbuf);
        free(cbuf);
        return DFK_E_OOM;
    }
    j.acc[0] = *a;
    for (b = 1; b < nb; b++) {
        double* f = fbuf ? fbuf + per * (size_t)nf * (b - 1) : NULL;
        if (aggs & (DFK_AGG_SUM | DFK_AGG_MEAN)) {
            j.acc[b].sum = f;
            f += per;
        }
        if (aggs & DFK_AGG_MIN) {
            j.acc[b].min = f;
            f += per;
        }
        if (aggs & DFK_AGG_MAX)
            j.acc[b].max = f;
        if (cbuf)
            j.acc[b].cnt = cbuf + per * (b - 1);
    }
    j.kp = kp;
    j.xf = xf;
    j.mask = mask;
    j.n = n;
    j.ngroups = ngroups;
    j.nblocks = nb;
    j.aggs = aggs;
    dyn_par_run(dyn_par_plan(nb, 1, 0), dfk_blk_worker, &j);
    for (b = 1; b < nb; b++) {
        const dfk_acc_t* p = &j.acc[b];
        for (g = 0; g < ngroups; g++) {
            if (aggs & (DFK_AGG_SUM | DFK_AGG_MEAN))
                a->sum[g] += p->sum[g];
            if (aggs & DFK_AGG_MIN)
                if (p->min[g] < a->min[g] || a->min[g] != a->min[g])
                    a->min[g] = p->min[g];
            if (aggs & DFK_AGG_MAX)
                if (p->max[g] > a->max[g] || a->max[g] != a->max[g])
                    a->max[g] = p->max[g];
            if (aggs & (DFK_AGG_COUNT | DFK_AGG_MEAN))
                a->cnt[g] += p->cnt[g];
        }
    }
    free(j.acc);
    free(fbuf);
    free(cbuf);
    return DFK_OK;
}

static int dfk_group_scan_i32(const int32_t* kp, const double* xf, uint32_t n, const uint8_t* mask,
    uint32_t ngroups, unsigned aggs, const dfk_acc_t* a)
{
    if (n >= 2 * DFK_BLOCK_ROWS && ngroups <= DFK_BLOCK_MAX_GROUPS)
        return dfk_group_scan_blocked(kp, xf, n, mask, ngroups, aggs, a);
    dfk_scan_i32_any(kp, xf, n, mask, ngroups, aggs, a);
    return DFK_OK;
}

static void dfk_scan_i32_any(const int32_t* kp, const double* xf, uint32_t n, const uint8_t* mask,
    uint32_t ngroups, unsigned aggs, const dfk_acc_t* a)
{
#define DFK_SCAN_I32(AGGS)                                       \
    do {                                                         \
        if (mask)                                                \
            dfk_scan_i32(kp, xf, n, mask, ngroups, (AGGS), a);   \
        else if (xf)                                             \
            dfk_scan_i32(kp, xf, n, NULL, ngroups, (AGGS), a);   \
        else                                                     \
            dfk_scan_i32(kp, NULL, n, NULL, ngroups, (AGGS), a); \
    } while (0)
    switch (aggs) {
    case DFK_AGG_SUM:
        DFK_SCAN_I32(DFK_AGG_SUM);
        break;
    case DFK_AGG_COUNT:
        DFK_SCAN_I32(DFK_AGG_COUNT);
        break;
    case DFK_AGG_MEAN:
        DFK_SCAN_I32(DFK_AGG_MEAN);
        break;
    case DFK_AGG_SUM | DFK_AGG_MEAN | DFK_AGG_COUNT:
        DFK_SCAN_I32(DFK_AGG_SUM | DFK_AGG_MEAN | DFK_AGG_COUNT);
        break;
    default:
        DFK_SCAN_I32(aggs);
        break;
    }
#undef DFK_SCAN_I32
}

int dfk_group_scan(DFType kt, const void* kp, DFType vt, const void* vp, uint32_t n,
    const uint8_t* mask, uint32_t ngroups, unsigned aggs, const dfk_acc_t* a)
{
    uint32_t i;
    uint32_t* gk;
    if (aggs & (DFK_AGG_SUM | DFK_AGG_MEAN))
        memset(a->sum, 0, (size_t)ngroups * sizeof(double));
    if (aggs & DFK_AGG_MIN)
        for (i = 0; i < ngroups; i++)
            a->min[i] = (double)NAN;
    if (aggs & DFK_AGG_MAX)
        for (i = 0; i < ngroups; i++)
            a->max[i] = (double)NAN;
    if (aggs & (DFK_AGG_COUNT | DFK_AGG_MEAN))
        memset(a->cnt, 0, (size_t)ngroups * sizeof(uint32_t));
    if ((kt == DF_I32 || kt == DF_STR) && (vt == DF_F64 || !vp || aggs == DFK_AGG_COUNT))
        return dfk_group_scan_i32((const int32_t*)kp, aggs == DFK_AGG_COUNT ? NULL : (const double*)vp, n, mask, ngroups, aggs, a);
    gk = dfk_keys_u32(kt, kp, n);
#define DFK_SCAN(AGGS)                                                    \
    do {                                                                  \
        if (vt == DF_F64 && vp && mask)                                   \
            dfk_scan(kt, kp, vt, vp, n, mask, ngroups, gk, (AGGS), 1, a); \
        else if (vt == DF_F64 && vp)                                      \
            dfk_scan(kt, kp, vt, vp, n, NULL, ngroups, gk, (AGGS), 1, a); \
        else                                                              \
            dfk_scan(kt, kp, vt, vp, n, mask, ngroups, gk, (AGGS), 0, a); \
    } while (0)
    switch (aggs) {
    case DFK_AGG_SUM:
        DFK_SCAN(DFK_AGG_SUM);
        break;
    case DFK_AGG_COUNT:
        dfk_scan(kt, kp, vt, vp, n, mask, ngroups, gk, DFK_AGG_COUNT, 0, a);
        break;
    case DFK_AGG_MEAN:
        DFK_SCAN(DFK_AGG_MEAN);
        break;
    case DFK_AGG_MIN:
        DFK_SCAN(DFK_AGG_MIN);
        break;
    case DFK_AGG_MAX:
        DFK_SCAN(DFK_AGG_MAX);
        break;
    default:
        DFK_SCAN(aggs);
        break;
    }
#undef DFK_SCAN
    free(gk);
    return DFK_OK;
}

void dfk_group_finish(unsigned agg, const dfk_acc_t* a, uint32_t nkeys, double* out)
{
    uint32_t i;
    switch (agg) {
    case DFK_AGG_SUM:
        if (out != a->sum)
            memcpy(out, a->sum, (size_t)nkeys * sizeof(double));
        break;
    case DFK_AGG_MEAN:
        for (i = 0; i < nkeys; i++)
            out[i] = a->cnt[i] ? a->sum[i] / a->cnt[i] : (double)NAN;
        break;
    case DFK_AGG_COUNT:
        for (i = 0; i < nkeys; i++)
            out[i] = (double)a->cnt[i];
        break;
    case DFK_AGG_MIN:
        for (i = 0; i < nkeys; i++)
            out[i] = a->min[i];
        break;
    default:
        for (i = 0; i < nkeys; i++)
            out[i] = a->max[i];
        break;
    }
}

uint32_t dfk_keys_present(const int32_t* kp, uint32_t n, uint32_t nkeys, uint8_t* seen)
{
    uint32_t i, g, have = 0;
    memset(seen, 0, nkeys);
    for (i = 0; i < n && have < nkeys; i++) {
        g = (uint32_t)kp[i];
        if (g < nkeys && !seen[g]) {
            seen[g] = 1;
            have++;
        }
    }
    return have;
}
