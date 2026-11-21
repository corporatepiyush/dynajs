#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-hyperloglog.h"

struct dyn_hll {
    uint8_t* reg;
    uint32_t p, m;
    double cached;
    int dirty;
};

dyn_hll_t* dyn_hll_new(uint32_t precision)
{
    dyn_hll_t* h;
    if (precision < DYN_HLL_MIN_PRECISION || precision > DYN_HLL_MAX_PRECISION)
        return NULL;
    h = (dyn_hll_t*)ds_calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->p = precision;
    h->m = 1u << precision;
    h->reg = (uint8_t*)ds_calloc(h->m, 1);
    if (!h->reg) {
        DYN_DS_FREE(h);
        return NULL;
    }
    return h;
}

void dyn_hll_free(dyn_hll_t* h)
{
    if (!h)
        return;
    DYN_DS_FREE(h->reg);
    DYN_DS_FREE(h);
}

void dyn_hll_add_hash(dyn_hll_t* h, uint64_t hash)
{
    uint32_t idx = (uint32_t)(hash >> (64 - h->p));
    uint64_t w = (hash << h->p) | ((uint64_t)1 << (h->p - 1));
    uint8_t rank = 1;
    while ((w & ((uint64_t)1 << 63)) == 0) {
        rank++;
        w <<= 1;
    }
    if (rank > h->reg[idx]) {
        h->reg[idx] = rank;
        h->dirty = 1;
    }
}

void dyn_hll_add(dyn_hll_t* h, const char* k, size_t n)
{
    dyn_hll_add_hash(h, dyn_ds_xxh64((const uint8_t*)k, n, 0));
}

static const double dyn_hll_inv_pow2[256] = {
    1.0 / (double)(1ULL << 0),
    1.0 / (double)(1ULL << 1),
    1.0 / (double)(1ULL << 2),
    1.0 / (double)(1ULL << 3),
    1.0 / (double)(1ULL << 4),
    1.0 / (double)(1ULL << 5),
    1.0 / (double)(1ULL << 6),
    1.0 / (double)(1ULL << 7),
    1.0 / (double)(1ULL << 8),
    1.0 / (double)(1ULL << 9),
    1.0 / (double)(1ULL << 10),
    1.0 / (double)(1ULL << 11),
    1.0 / (double)(1ULL << 12),
    1.0 / (double)(1ULL << 13),
    1.0 / (double)(1ULL << 14),
    1.0 / (double)(1ULL << 15),
    1.0 / (double)(1ULL << 16),
    1.0 / (double)(1ULL << 17),
    1.0 / (double)(1ULL << 18),
    1.0 / (double)(1ULL << 19),
    1.0 / (double)(1ULL << 20),
    1.0 / (double)(1ULL << 21),
    1.0 / (double)(1ULL << 22),
    1.0 / (double)(1ULL << 23),
    1.0 / (double)(1ULL << 24),
    1.0 / (double)(1ULL << 25),
    1.0 / (double)(1ULL << 26),
    1.0 / (double)(1ULL << 27),
    1.0 / (double)(1ULL << 28),
    1.0 / (double)(1ULL << 29),
    1.0 / (double)(1ULL << 30),
    1.0 / (double)(1ULL << 31),
    1.0 / (double)(1ULL << 32),
    1.0 / (double)(1ULL << 33),
    1.0 / (double)(1ULL << 34),
    1.0 / (double)(1ULL << 35),
    1.0 / (double)(1ULL << 36),
    1.0 / (double)(1ULL << 37),
    1.0 / (double)(1ULL << 38),
    1.0 / (double)(1ULL << 39),
    1.0 / (double)(1ULL << 40),
    1.0 / (double)(1ULL << 41),
    1.0 / (double)(1ULL << 42),
    1.0 / (double)(1ULL << 43),
    1.0 / (double)(1ULL << 44),
    1.0 / (double)(1ULL << 45),
    1.0 / (double)(1ULL << 46),
    1.0 / (double)(1ULL << 47),
    1.0 / (double)(1ULL << 48),
    1.0 / (double)(1ULL << 49),
    1.0 / (double)(1ULL << 50),
    1.0 / (double)(1ULL << 51),
    1.0 / (double)(1ULL << 52),
    1.0 / (double)(1ULL << 53),
    1.0 / (double)(1ULL << 54),
    1.0 / (double)(1ULL << 55),
    1.0 / (double)(1ULL << 56),
    1.0 / (double)(1ULL << 57),
    1.0 / (double)(1ULL << 58),
    1.0 / (double)(1ULL << 59),
    1.0 / (double)(1ULL << 60),
    1.0 / (double)(1ULL << 61),
    1.0 / (double)(1ULL << 62),
    1.0 / (double)(1ULL << 63),
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
};

double dyn_hll_count(dyn_hll_t* h)
{
    double m = (double)h->m, sum = 0.0, alpha, est;
    uint32_t i, zeros = 0;
    double s[DYN_HLL_NACC];
    uint32_t z[DYN_HLL_NACC], lim, j;
    const uint8_t* reg = h->reg;

    if (!h->dirty)
        return h->cached;

    for (j = 0; j < DYN_HLL_NACC; j++) {
        s[j] = 0.0;
        z[j] = 0;
    }
    lim = h->m & ~(uint32_t)(DYN_HLL_NACC - 1);
    for (i = 0; i < lim; i += DYN_HLL_NACC)
        for (j = 0; j < DYN_HLL_NACC; j++) {
            s[j] += dyn_hll_inv_pow2[reg[i + j]];
            z[j] += (reg[i + j] == 0);
        }
    for (; i < h->m; i++) {
        s[0] += dyn_hll_inv_pow2[reg[i]];
        z[0] += (reg[i] == 0);
    }
    for (j = 0; j < DYN_HLL_NACC; j++) {
        sum += s[j];
        zeros += z[j];
    }
    if (h->m == 16)
        alpha = 0.673;
    else if (h->m == 32)
        alpha = 0.697;
    else if (h->m == 64)
        alpha = 0.709;
    else
        alpha = 0.7213 / (1.0 + 1.079 / m);

    est = alpha * m * m / sum;
    if (est <= 2.5 * m && zeros > 0)
        est = m * log(m / (double)zeros);
    ((dyn_hll_t*)h)->cached = est;
    ((dyn_hll_t*)h)->dirty = 0;
    return est;
}

uint32_t dyn_hll_precision(const dyn_hll_t* h) { return h->p; }

uint32_t dyn_hll_registers(const dyn_hll_t* h) { return h->m; }

int dyn_hll_merge(dyn_hll_t* a, const dyn_hll_t* b)
{
    uint32_t i;
    if (a->p != b->p)
        return -1;
    for (i = 0; i < a->m; i++)
        if (b->reg[i] > a->reg[i])
            a->reg[i] = b->reg[i];
    a->dirty = 1;
    return 0;
}

const uint8_t* dyn_hll_data(const dyn_hll_t* h) { return h->reg; }

uint8_t* dyn_hll_data_mut(dyn_hll_t* h)
{
    h->dirty = 1;
    return h->reg;
}

int dyn_hll_regs_valid(const dyn_hll_t* h)
{
    uint32_t i, max = 64 - h->p + 1;
    for (i = 0; i < h->m; i++)
        if (h->reg[i] > max)
            return -1;
    return 0;
}

uint8_t* dyn_hll_swap_regs(dyn_hll_t* h, uint8_t* reg)
{
    uint8_t* old = h->reg;
    h->reg = reg;
    h->dirty = 1;
    return old;
}
