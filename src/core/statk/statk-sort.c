#include "statk-int.h"

#include <stdlib.h>
#include <string.h>

static inline uint64_t st_key(double v)
{
    uint64_t u;
    memcpy(&u, &v, sizeof u);
    if (u == 0x8000000000000000ull)
        u = 0;
    return (u >> 63) ? ~u : (u | 0x8000000000000000ull);
}

static void st_insertion(const double* x, uint32_t n, uint32_t* idx)
{
    uint32_t i, j;
    for (i = 1; i < n; i++) {
        uint32_t v = idx[i];
        double xv = x[v];
        for (j = i; j > 0 && x[idx[j - 1]] > xv; j--)
            idx[j] = idx[j - 1];
        idx[j] = v;
    }
}

#define ST_RADIX16_MIN 32768u

static int st_argsort8(const double* x, uint32_t n, uint32_t* idx)
{
    uint64_t *k0, *k1, *src_k, *dst_k;
    uint32_t *i1, *src_i, *dst_i, i, b;
    uint32_t cnt[8][256];
    int pass;
    k0 = (uint64_t*)malloc((size_t)n * sizeof(uint64_t));
    k1 = (uint64_t*)malloc((size_t)n * sizeof(uint64_t));
    i1 = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    if (!k0 || !k1 || !i1) {
        free(k0);
        free(k1);
        free(i1);
        return DYN_ST_E_OOM;
    }
    memset(cnt, 0, sizeof cnt);
    for (i = 0; i < n; i++) {
        uint64_t k = st_key(x[i]);
        k0[i] = k;
        for (pass = 0; pass < 8; pass++)
            cnt[pass][(k >> (8 * pass)) & 0xffu]++;
    }
    src_k = k0;
    dst_k = k1;
    src_i = idx;
    dst_i = i1;
    for (pass = 0; pass < 8; pass++) {
        unsigned sh = (unsigned)pass * 8u;
        uint32_t sum = 0, *c = cnt[pass];
        if (c[(k0[0] >> sh) & 0xffu] == n)
            continue;
        for (b = 0; b < 256u; b++) {
            uint32_t t = c[b];
            c[b] = sum;
            sum += t;
        }
        for (i = 0; i < n; i++) {
            uint32_t d = c[(src_k[i] >> sh) & 0xffu]++;
            dst_k[d] = src_k[i];
            dst_i[d] = src_i[i];
        }
        {
            uint64_t* tk = src_k;
            uint32_t* ti = src_i;
            src_k = dst_k;
            dst_k = tk;
            src_i = dst_i;
            dst_i = ti;
        }
    }
    if (src_i != idx)
        memcpy(idx, src_i, (size_t)n * sizeof(uint32_t));
    free(k0);
    free(k1);
    free(i1);
    return DYN_ST_OK;
}

int dyn_st_argsort(const double* x, uint32_t n, uint32_t* idx)
{
    uint64_t *k0, *k1;
    uint32_t *i1, *src_i, *dst_i, i;
    uint64_t *src_k, *dst_k;
    uint32_t* cnt;
    int pass;
    for (i = 0; i < n; i++)
        idx[i] = i;
    if (n < 64) {
        st_insertion(x, n, idx);
        return DYN_ST_OK;
    }
    if (n < ST_RADIX16_MIN)
        return st_argsort8(x, n, idx);
    k0 = (uint64_t*)malloc((size_t)n * sizeof(uint64_t));
    k1 = (uint64_t*)malloc((size_t)n * sizeof(uint64_t));
    i1 = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    cnt = (uint32_t*)malloc(65536u * sizeof(uint32_t));
    if (!k0 || !k1 || !i1 || !cnt) {
        free(k0);
        free(k1);
        free(i1);
        free(cnt);
        return DYN_ST_E_OOM;
    }
    for (i = 0; i < n; i++)
        k0[i] = st_key(x[i]);
    src_k = k0;
    dst_k = k1;
    src_i = idx;
    dst_i = i1;
    for (pass = 0; pass < 4; pass++) {
        unsigned sh = (unsigned)pass * 16u;
        uint32_t sum = 0, first;
        memset(cnt, 0, 65536u * sizeof(uint32_t));
        for (i = 0; i < n; i++)
            cnt[(src_k[i] >> sh) & 0xffffu]++;
        first = (uint32_t)((src_k[0] >> sh) & 0xffffu);
        if (cnt[first] == n)
            continue;
        for (i = 0; i < 65536u; i++) {
            uint32_t c = cnt[i];
            cnt[i] = sum;
            sum += c;
        }
        for (i = 0; i < n; i++) {
            uint32_t d = cnt[(src_k[i] >> sh) & 0xffffu]++;
            dst_k[d] = src_k[i];
            dst_i[d] = src_i[i];
        }
        {
            uint64_t* tk = src_k;
            uint32_t* ti = src_i;
            src_k = dst_k;
            dst_k = tk;
            src_i = dst_i;
            dst_i = ti;
        }
    }
    if (src_i != idx)
        memcpy(idx, src_i, (size_t)n * sizeof(uint32_t));
    free(k0);
    free(k1);
    free(i1);
    free(cnt);
    return DYN_ST_OK;
}

int dyn_st_sort_copy(const double* x, uint32_t n, double* out)
{
    uint32_t* idx;
    uint32_t i;
    int rc;
    if (n == 0)
        return DYN_ST_OK;
    idx = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    if (!idx)
        return DYN_ST_E_OOM;
    rc = dyn_st_argsort(x, n, idx);
    if (rc == DYN_ST_OK)
        for (i = 0; i < n; i++)
            out[i] = x[idx[i]];
    free(idx);
    return rc;
}

int dyn_st_rank(const double* x, uint32_t n, dyn_st_rank_t method, double* out, double* tie_term)
{
    uint32_t *idx, i = 0, dense = 0;
    double tt = 0.0;
    int rc;
    if (tie_term)
        *tie_term = 0.0;
    if (n == 0)
        return DYN_ST_OK;
    idx = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    if (!idx)
        return DYN_ST_E_OOM;
    rc = dyn_st_argsort(x, n, idx);
    if (rc != DYN_ST_OK) {
        free(idx);
        return rc;
    }
    while (i < n) {
        uint32_t j = i, k;
        double v, t;
        while (j + 1 < n && x[idx[j + 1]] == x[idx[i]])
            j++;
        t = (double)(j - i + 1);
        tt += t * t * t - t;
        dense++;
        switch (method) {
        case DYN_ST_RANK_MIN:
            v = (double)i + 1.0;
            break;
        case DYN_ST_RANK_MAX:
            v = (double)j + 1.0;
            break;
        case DYN_ST_RANK_DENSE:
            v = (double)dense;
            break;
        case DYN_ST_RANK_ORDINAL:
            v = 0.0;
            break;
        case DYN_ST_RANK_AVERAGE:
        default:
            v = 0.5 * ((double)i + (double)j) + 1.0;
            break;
        }
        for (k = i; k <= j; k++)
            out[idx[k]] = method == DYN_ST_RANK_ORDINAL ? (double)k + 1.0 : v;
        i = j + 1;
    }
    if (tie_term)
        *tie_term = tt;
    free(idx);
    return DYN_ST_OK;
}
