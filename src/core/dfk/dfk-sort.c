#include "dfk.h"
#include "../dyn-par.h"

static inline int dfo_before(const DfoItem* a, const DfoItem* b, int desc)
{
    int na = a->key != a->key, nb = b->key != b->key;

    if (na | nb) {
        if (na != nb)
            return nb;
    } else if (a->key != b->key) {
        return desc ? (a->key > b->key) : (a->key < b->key);
    }
    return a->idx < b->idx;
}

static inline void dfo_swap(DfoItem* a, DfoItem* b)
{
    DfoItem t = *a;
    *a = *b;
    *b = t;
}

static int dfo_cmp_asc(const void* pa, const void* pb)
{
    const DfoItem *a = pa, *b = pb;
    int na = isnan(a->key), nb = isnan(b->key);

    if (na | nb) {
        if (na != nb)
            return na - nb;
    } else if (a->key < b->key) {
        return -1;
    } else if (a->key > b->key) {
        return 1;
    }
    return a->idx < b->idx ? -1 : (a->idx > b->idx ? 1 : 0);
}

static int dfo_cmp_desc(const void* pa, const void* pb)
{
    const DfoItem *a = pa, *b = pb;
    int na = isnan(a->key), nb = isnan(b->key);

    if (na | nb) {
        if (na != nb)
            return na - nb;
    } else if (a->key > b->key) {
        return -1;
    } else if (a->key < b->key) {
        return 1;
    }
    return a->idx < b->idx ? -1 : (a->idx > b->idx ? 1 : 0);
}

static uint32_t dfo_pivot(DfoItem* it, uint32_t n, int desc)
{
    uint32_t m = n >> 1, last = n - 1;

    if (dfo_before(&it[m], &it[0], desc))
        dfo_swap(&it[m], &it[0]);
    if (dfo_before(&it[last], &it[m], desc))
        dfo_swap(&it[last], &it[m]);
    if (dfo_before(&it[m], &it[0], desc))
        dfo_swap(&it[m], &it[0]);
    return m;
}

static uint32_t dfo_partition(DfoItem* it, uint32_t n, uint32_t p, int desc)
{
    DfoItem pivot = it[p];
    uint32_t i = 0, j = n - 1;

    for (;;) {
        while (dfo_before(&it[i], &pivot, desc))
            i++;
        while (dfo_before(&pivot, &it[j], desc))
            j--;
        if (i >= j)
            return j;
        dfo_swap(&it[i], &it[j]);
        i++;
        if (j == 0)
            return 0;
        j--;
    }
}

static void dfo_insertion(DfoItem* it, uint32_t n, int desc)
{
    uint32_t i, j;

    for (i = 1; i < n; i++) {
        DfoItem v = it[i];
        for (j = i; j > 0 && dfo_before(&v, &it[j - 1], desc); j--)
            it[j] = it[j - 1];
        it[j] = v;
    }
}

void dfo_sort(DfoItem* it, uint32_t n, int desc, int depth)
{
    while (n > DFO_SMALL) {
        uint32_t p;
        if (depth-- <= 0) {
            qsort(it, n, sizeof(*it), desc ? dfo_cmp_desc : dfo_cmp_asc);
            return;
        }
        p = dfo_partition(it, n, dfo_pivot(it, n, desc), desc);
        dfo_sort(it, p + 1, desc, depth);
        it += p + 1;
        n -= p + 1;
    }
    dfo_insertion(it, n, desc);
}

static int dfo_depth(uint32_t n)
{
    int depth = 2;

    while (n >>= 1)
        depth += 2;
    return depth;
}

void dfo_select(DfoItem* it, uint32_t n, uint32_t k, int desc)
{
    int depth = dfo_depth(n);

    while (n > DFO_SMALL) {
        uint32_t p;
        if (depth-- <= 0) {
            dfo_sort(it, n, desc, dfo_depth(n));
            return;
        }
        p = dfo_partition(it, n, dfo_pivot(it, n, desc), desc);
        if (k <= p) {
            n = p + 1;
        } else {
            it += p + 1;
            n -= p + 1;
            k -= p + 1;
        }
    }
    dfo_insertion(it, n, desc);
}

#define DFO_GATHER(CTYPE)                     \
    do {                                      \
        const CTYPE* x = p;                   \
        if (mask) {                           \
            for (i = 0; i < span; i++)        \
                if (mask[i]) {                \
                    it[n].key = (double)x[i]; \
                    it[n].idx = i;            \
                    n++;                      \
                }                             \
        } else {                              \
            for (i = 0; i < span; i++) {      \
                it[i].key = (double)x[i];     \
                it[i].idx = i;                \
            }                                 \
            n = span;                         \
        }                                     \
    } while (0)

uint32_t dfo_gather(const void* p, DFType t, const uint8_t* mask,
    uint32_t span, DfoItem* it)
{
    uint32_t i, n = 0;

    switch ((int)(t)) {
    case DF_F64:
        DFO_GATHER(double);
        break;
    case DF_F32:
        DFO_GATHER(float);
        break;
    case DF_I32:
        DFO_GATHER(int32_t);
        break;
    case DF_U32:
        DFO_GATHER(uint32_t);
        break;
    case DF_I16:
        DFO_GATHER(int16_t);
        break;
    case DF_U16:
        DFO_GATHER(uint16_t);
        break;
    case DF_I8:
        DFO_GATHER(int8_t);
        break;
    default:
        DFO_GATHER(uint8_t);
        break;
    }
    return n;
}

#undef DFO_GATHER

static int dfo_radix(DfoItem* it, uint32_t n, int desc)
{
    uint64_t *ka = NULL, *kb = NULL, *ks, *kd;
    uint32_t *ia = NULL, *ib = NULL, *is, *id;
    DfoItem* nanv = NULL;
    uint32_t (*hist)[256] = NULL, i, p, nn = 0, nz = 0;
    int negzero = 0;

    if (n < DFO_RADIX_SORT_MIN)
        return -1;
    ka = malloc((size_t)n * sizeof(*ka));
    kb = malloc((size_t)n * sizeof(*kb));
    ia = malloc((size_t)n * sizeof(*ia));
    ib = malloc((size_t)n * sizeof(*ib));
    hist = calloc(8, sizeof(*hist));
    if (!ka || !kb || !ia || !ib || !hist)
        goto decline;

    for (i = 0; i < n; i++) {
        uint64_t k, raw;
        if (it[i].key != it[i].key) {
            nz++;
            continue;
        }
        memcpy(&raw, &it[i].key, sizeof(raw));
        negzero |= (raw == 0x8000000000000000ULL);
        k = dfo_skey(it[i].key);
        if (desc)
            k = ~k;
        ka[nn] = k;
        ia[nn] = it[i].idx;
        nn++;
        hist[0][k & 0xff]++;
        hist[1][(k >> 8) & 0xff]++;
        hist[2][(k >> 16) & 0xff]++;
        hist[3][(k >> 24) & 0xff]++;
        hist[4][(k >> 32) & 0xff]++;
        hist[5][(k >> 40) & 0xff]++;
        hist[6][(k >> 48) & 0xff]++;
        hist[7][(k >> 56) & 0xff]++;
    }
    if (negzero)
        goto decline;
    if (nz) {
        nanv = malloc((size_t)nz * sizeof(*nanv));
        if (!nanv)
            goto decline;
        nz = 0;
        for (i = 0; i < n; i++)
            if (it[i].key != it[i].key)
                nanv[nz++] = it[i];
    }

    ks = ka;
    kd = kb;
    is = ia;
    id = ib;
    for (p = 0; p < 8; p++) {
        uint32_t *h = hist[p], sum = 0, shift = p * 8;
        if (nn && h[(ks[0] >> shift) & 0xff] == nn)
            continue;
        for (i = 0; i < 256; i++) {
            uint32_t c = h[i];
            h[i] = sum;
            sum += c;
        }
        for (i = 0; i < nn; i++) {
            uint32_t d = h[(ks[i] >> shift) & 0xff]++;
            kd[d] = ks[i];
            id[d] = is[i];
        }
        {
            uint64_t* tk = ks;
            ks = kd;
            kd = tk;
        }
        {
            uint32_t* ti = is;
            is = id;
            id = ti;
        }
    }
    for (i = 0; i < nn; i++) {
        it[i].key = dfo_unskey(desc ? ~ks[i] : ks[i]);
        it[i].idx = is[i];
    }
    for (i = 0; i < nz; i++)
        it[nn + i] = nanv[i];
    free(nanv);
    free(ka);
    free(kb);
    free(ia);
    free(ib);
    free(hist);
    return 0;
decline:
    free(nanv);
    free(ka);
    free(kb);
    free(ia);
    free(ib);
    free(hist);
    return -1;
}

typedef struct {
    DfoItem* it;
    uint32_t n;
    int desc;
    uint64_t *ka, *kb;
    uint32_t *ia, *ib;
    DfoItem* nanv;
    uint32_t (*hist)[256];
    uint32_t *cnt, *nanc, *nzf;
    uint32_t nn, nz;
    int negzero;
    int skip;
} dfo_par_t;

static void dfo_radix_worker(void* arg, unsigned w, unsigned nw, dyn_par_t* par)
{
    dfo_par_t* x = (dfo_par_t*)arg;
    uint32_t lo = (uint32_t)((uint64_t)x->n * w / nw);
    uint32_t hi = (uint32_t)((uint64_t)x->n * (w + 1) / nw);
    uint32_t* h = x->hist[w];
    uint64_t *ks = x->ka, *kd = x->kb;
    uint32_t *is = x->ia, *id = x->ib;
    uint32_t i, c = 0, z = 0, p;
    int nzero = 0;

    for (i = lo; i < hi; i++) {
        if (x->it[i].key != x->it[i].key) {
            z++;
        } else {
            uint64_t raw;
            memcpy(&raw, &x->it[i].key, sizeof(raw));
            nzero |= (raw == 0x8000000000000000ULL);
            c++;
        }
    }
    x->cnt[w] = c;
    x->nanc[w] = z;
    x->nzf[w] = (uint32_t)nzero;
    dyn_par_barrier(par);
    if (w == 0) {
        uint32_t k, sc = 0, sz = 0;
        for (k = 0; k < nw; k++) {
            uint32_t t = x->cnt[k], u = x->nanc[k];
            x->cnt[k] = sc;
            x->nanc[k] = sz;
            sc += t;
            sz += u;
        }
        x->nn = sc;
        x->nz = sz;
        for (k = 0; k < nw; k++)
            x->negzero |= (int)x->nzf[k];
    }
    dyn_par_barrier(par);
    if (x->negzero)
        return;
    {
        uint32_t o = x->cnt[w], q = x->nanc[w];
        for (i = lo; i < hi; i++) {
            if (x->it[i].key != x->it[i].key) {
                x->nanv[q++] = x->it[i];
            } else {
                uint64_t k = dfo_skey(x->it[i].key);
                if (x->desc)
                    k = ~k;
                ks[o] = k;
                is[o] = x->it[i].idx;
                o++;
            }
        }
    }
    dyn_par_barrier(par);
    lo = (uint32_t)((uint64_t)x->nn * w / nw);
    hi = (uint32_t)((uint64_t)x->nn * (w + 1) / nw);
    for (p = 0; p < 8; p++) {
        uint32_t shift = p * 8;
        memset(h, 0, 256 * sizeof(*h));
        for (i = lo; i < hi; i++)
            h[(ks[i] >> shift) & 0xff]++;
        dyn_par_barrier(par);
        if (w == 0) {
            uint32_t d, k, sum = 0;
            x->skip = 0;
            if (x->nn) {
                uint32_t d0 = (uint32_t)((ks[0] >> shift) & 0xff), tot = 0;
                for (k = 0; k < nw; k++)
                    tot += x->hist[k][d0];
                x->skip = tot == x->nn;
            }
            if (!x->skip)
                for (d = 0; d < 256; d++)
                    for (k = 0; k < nw; k++) {
                        uint32_t t = x->hist[k][d];
                        x->hist[k][d] = sum;
                        sum += t;
                    }
        }
        dyn_par_barrier(par);
        if (x->skip)
            continue;
        for (i = lo; i < hi; i++) {
            uint32_t d = h[(ks[i] >> shift) & 0xff]++;
            kd[d] = ks[i];
            id[d] = is[i];
        }
        dyn_par_barrier(par);
        {
            uint64_t* tk = ks;
            ks = kd;
            kd = tk;
        }
        {
            uint32_t* ti = is;
            is = id;
            id = ti;
        }
    }
    for (i = lo; i < hi; i++) {
        x->it[i].key = dfo_unskey(x->desc ? ~ks[i] : ks[i]);
        x->it[i].idx = is[i];
    }
    lo = (uint32_t)((uint64_t)x->nz * w / nw);
    hi = (uint32_t)((uint64_t)x->nz * (w + 1) / nw);
    for (i = lo; i < hi; i++)
        x->it[x->nn + i] = x->nanv[i];
}

static int dfo_radix_par(DfoItem* it, uint32_t n, int desc, unsigned nw)
{
    dfo_par_t x;
    int rc = -1;

    memset(&x, 0, sizeof x);
    x.it = it;
    x.n = n;
    x.desc = desc;
    x.ka = malloc((size_t)n * sizeof(*x.ka));
    x.kb = malloc((size_t)n * sizeof(*x.kb));
    x.ia = malloc((size_t)n * sizeof(*x.ia));
    x.ib = malloc((size_t)n * sizeof(*x.ib));
    x.nanv = malloc((size_t)n * sizeof(*x.nanv));
    x.hist = calloc(nw, sizeof(*x.hist));
    x.cnt = calloc(nw, sizeof(*x.cnt));
    x.nanc = calloc(nw, sizeof(*x.nanc));
    x.nzf = calloc(nw, sizeof(*x.nzf));
    if (x.ka && x.kb && x.ia && x.ib && x.nanv && x.hist && x.cnt && x.nanc && x.nzf) {
        dyn_par_run(nw, dfo_radix_worker, &x);
        rc = x.negzero ? -1 : 0;
    }
    free(x.ka);
    free(x.kb);
    free(x.ia);
    free(x.ib);
    free(x.nanv);
    free(x.hist);
    free(x.cnt);
    free(x.nanc);
    free(x.nzf);
    return rc;
}

static int dfo_radix_any(DfoItem* it, uint32_t n, int desc)
{
    unsigned nw = n < DFO_RADIX_MIN ? 1u : dyn_par_plan(n, DFO_RADIX_PAR_ROWS_PER_WORKER, 0);
    return nw >= DFO_RADIX_PAR_MIN_WORKERS ? dfo_radix_par(it, n, desc, nw) : dfo_radix(it, n, desc);
}

void dfo_sort_any(DfoItem* it, uint32_t n, int desc)
{
    if (n > 1 && dfo_radix_any(it, n, desc) != 0)
        dfo_sort(it, n, desc, dfo_depth(n));
}

void dfo_top(DfoItem* it, uint32_t n, uint32_t k, int desc)
{
    if (k == 0)
        return;
    if ((uint64_t)k * DFO_TOP_SEL_RATIO >= n) {
        dfo_sort_any(it, n, desc);
        return;
    }
    dfo_select(it, n, k - 1, desc);
    dfo_sort(it, k, desc, dfo_depth(k));
}

static uint64_t* dfo_lsd(uint64_t* ks, uint64_t* kd, uint32_t* is, uint32_t* id,
    uint32_t n, uint32_t (*hist)[256], uint32_t** out)
{
    uint32_t i, p;

    for (i = 0; i < n; i++) {
        uint64_t k = ks[i];
        hist[0][k & 0xff]++;
        hist[1][(k >> 8) & 0xff]++;
        hist[2][(k >> 16) & 0xff]++;
        hist[3][(k >> 24) & 0xff]++;
        hist[4][(k >> 32) & 0xff]++;
        hist[5][(k >> 40) & 0xff]++;
        hist[6][(k >> 48) & 0xff]++;
        hist[7][(k >> 56) & 0xff]++;
    }
    for (p = 0; p < 8; p++) {
        uint32_t *h = hist[p], sum = 0, shift = p * 8;
        if (h[(ks[0] >> shift) & 0xff] == n)
            continue;
        for (i = 0; i < 256; i++) {
            uint32_t c = h[i];
            h[i] = sum;
            sum += c;
        }
        if (is) {
            for (i = 0; i < n; i++) {
                uint32_t d = h[(ks[i] >> shift) & 0xff]++;
                kd[d] = ks[i];
                id[d] = is[i];
            }
            {
                uint32_t* ti = is;
                is = id;
                id = ti;
            }
        } else {
            for (i = 0; i < n; i++)
                kd[h[(ks[i] >> shift) & 0xff]++] = ks[i];
        }
        {
            uint64_t* tk = ks;
            ks = kd;
            kd = tk;
        }
    }
    if (out)
        *out = is;
    return ks;
}

static double dfo_pair_key(const unsigned char* e)
{
    double k;

    memcpy(&k, e, sizeof(k));
    return k == 0.0 ? 0.0 : k;
}

static void dfo_pair_insertion(unsigned char* p, uint32_t n)
{
    unsigned char v[DFO_PAIR_SIZE];
    uint32_t i, j;

    for (i = 1; i < n; i++) {
        double k = dfo_pair_key(p + (size_t)i * DFO_PAIR_SIZE);
        if (!(k < dfo_pair_key(p + (size_t)(i - 1) * DFO_PAIR_SIZE)))
            continue;
        memcpy(v, p + (size_t)i * DFO_PAIR_SIZE, DFO_PAIR_SIZE);
        for (j = i; j > 0 && k < dfo_pair_key(p + (size_t)(j - 1) * DFO_PAIR_SIZE); j--)
            memcpy(p + (size_t)j * DFO_PAIR_SIZE, p + (size_t)(j - 1) * DFO_PAIR_SIZE, DFO_PAIR_SIZE);
        memcpy(p + (size_t)j * DFO_PAIR_SIZE, v, DFO_PAIR_SIZE);
    }
}

void dfo_pair_small_sort(void* base, uint32_t n)
{
    static const uint8_t gaps[4] = { 23, 10, 4, 1 };
    unsigned char *p = base, tmp[DFO_SMALL_RADIX_MIN * DFO_PAIR_SIZE];
    uint64_t k[DFO_SMALL_RADIX_MIN];
    uint8_t ix[DFO_SMALL_RADIX_MIN];
    uint32_t i, j, gi;

    if (n > DFO_SMALL_RADIX_MIN) {
        dfo_pair_insertion(p, n);
        return;
    }
    for (i = 0; i < n; i++) {
        k[i] = dfo_skey(dfo_pair_key(p + (size_t)i * DFO_PAIR_SIZE));
        ix[i] = (uint8_t)i;
    }
    for (gi = 0; gi < 4; gi++) {
        uint32_t g = gaps[gi];
        for (i = g; i < n; i++) {
            uint64_t kv = k[i];
            uint8_t iv = ix[i];
            for (j = i; j >= g && (k[j - g] > kv || (k[j - g] == kv && ix[j - g] > iv)); j -= g) {
                k[j] = k[j - g];
                ix[j] = ix[j - g];
            }
            k[j] = kv;
            ix[j] = iv;
        }
    }
    for (i = 0; i < n; i++)
        memcpy(tmp + (size_t)i * DFO_PAIR_SIZE, p + (size_t)ix[i] * DFO_PAIR_SIZE, DFO_PAIR_SIZE);
    memcpy(p, tmp, (size_t)n * DFO_PAIR_SIZE);
}

int dfo_pair_sort(void* base, uint32_t n, uint32_t min)
{
    unsigned char *p = base, *tmp;
    uint64_t* k;
    uint32_t *ix, *ord, (*hist)[256], i;

    if (n < min || n < DFO_SMALL_RADIX_MIN)
        return -1;
    k = malloc((size_t)n * 2 * sizeof(*k));
    ix = malloc((size_t)n * 2 * sizeof(*ix));
    tmp = malloc((size_t)n * DFO_PAIR_SIZE);
    hist = calloc(8, sizeof(*hist));
    if (!k || !ix || !tmp || !hist) {
        free(k);
        free(ix);
        free(tmp);
        free(hist);
        return -1;
    }
    for (i = 0; i < n; i++) {
        k[i] = dfo_skey(dfo_pair_key(p + (size_t)i * DFO_PAIR_SIZE));
        ix[i] = i;
    }
    dfo_lsd(k, k + n, ix, ix + n, n, hist, &ord);
    for (i = 0; i < n; i++)
        memcpy(tmp + (size_t)i * DFO_PAIR_SIZE, p + (size_t)ord[i] * DFO_PAIR_SIZE, DFO_PAIR_SIZE);
    memcpy(p, tmp, (size_t)n * DFO_PAIR_SIZE);
    free(k);
    free(ix);
    free(tmp);
    free(hist);
    return 0;
}

static void dfo_f64_small(double* a, uint32_t n)
{
    static const uint8_t gaps[4] = { 23, 10, 4, 1 };
    uint64_t k[DFO_SMALL_RADIX_MIN];
    uint32_t i, j, m = 0, gi;

    for (i = 0; i < n; i++)
        if (a[i] == a[i])
            k[m++] = dfo_skey(a[i]);
    for (i = n, j = n; i-- > 0;)
        if (a[i] != a[i])
            a[--j] = a[i];
    for (gi = 0; gi < 4; gi++) {
        uint32_t g = gaps[gi];
        for (i = g; i < m; i++) {
            uint64_t v = k[i];
            for (j = i; j >= g && k[j - g] > v; j -= g)
                k[j] = k[j - g];
            k[j] = v;
        }
    }
    for (i = 0; i < m; i++)
        a[i] = dfo_unskey(k[i]);
}

int dfo_f64_sort(double* a, uint32_t n)
{
    uint64_t *k, *ks;
    uint32_t (*hist)[256], i, j, m = 0;

    if (n < DFO_SMALL_RADIX_MIN) {
        dfo_f64_small(a, n);
        return 0;
    }
    k = malloc((size_t)n * 2 * sizeof(*k));
    hist = calloc(8, sizeof(*hist));
    if (!k || !hist) {
        free(k);
        free(hist);
        return -1;
    }
    for (i = 0; i < n; i++)
        if (a[i] == a[i])
            k[m++] = dfo_skey(a[i]);
    for (i = n, j = n; i-- > 0;)
        if (a[i] != a[i])
            a[--j] = a[i];
    if (m) {
        ks = dfo_lsd(k, k + m, NULL, NULL, m, hist, NULL);
        for (i = 0; i < m; i++)
            a[i] = dfo_unskey(ks[i]);
    }
    free(k);
    free(hist);
    return 0;
}

uint32_t dfo_valued(const DfoItem* it, uint32_t n)
{
    while (n > 0 && isnan(it[n - 1].key))
        n--;
    return n;
}
