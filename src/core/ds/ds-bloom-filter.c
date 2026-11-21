#include "ds-internal.h"
#include "ds-bloom-filter.h"

static inline void ds_bloom_probe_init(uint32_t m, uint64_t h1, uint64_t h2,
    uint32_t* bit, uint32_t* step,
    uint32_t* wmod)
{
    uint64_t w;
    *bit = (uint32_t)(h1 % m);
    *step = (uint32_t)(h2 % m);
    if (!*step)
        *step = 1;
    w = (UINT64_MAX % m) + 1;
    *wmod = (uint32_t)(w == m ? 0 : w);
}

static inline uint32_t ds_bloom_probe_next(uint32_t m, uint64_t* acc,
    uint64_t h2, uint32_t bit,
    uint32_t step, uint32_t wmod)
{
    uint64_t next = *acc + h2;
    uint64_t r = (uint64_t)bit + step;
    if (r >= m)
        r -= m;
    if (next < *acc) {
        r = r + m - wmod;
        if (r >= m)
            r -= m;
    }
    *acc = next;
    return (uint32_t)r;
}

static void ds_bloom_hashes(const char* s, size_t n, uint64_t* h1, uint64_t* h2)
{
    uint64_t a = 14695981039346656037ULL ^ 0x9E3779B97F4A7C15ULL;
    uint64_t c = ~a;
    size_t i;
    for (i = 0; i < n; i++) {
        a ^= (uint64_t)(unsigned char)s[i];
        a *= 1099511628211ULL;
        c = (c ^ (uint64_t)(unsigned char)s[i]) * 1099511628211ULL + 0x9E3779B9ULL;
    }
    *h1 = a;
    *h2 = c | 1;
}

int dyn_bloom_init(dyn_bloom_t* b, uint32_t nbits, uint32_t k)
{
    b->nbits = nbits;
    b->k = k;
    b->nwords = (nbits + 63) >> 6;
    b->words = (uint64_t*)DYN_DS_CALLOC(b->nwords ? b->nwords : 1, sizeof(uint64_t));
    return b->words ? 0 : -1;
}

void dyn_bloom_free(dyn_bloom_t* b)
{
    DYN_DS_FREE(b->words);
    b->words = NULL;
}

void dyn_bloom_add(dyn_bloom_t* b, const void* key, size_t n)
{
    uint64_t h1, h2;
    uint32_t i;
    ds_bloom_hashes((const char*)key, n, &h1, &h2);
    if (b->k <= 4) {
        for (i = 0; i < b->k; i++) {
            uint32_t bit = (uint32_t)((h1 + (uint64_t)i * h2) % b->nbits);
            b->words[bit >> 6] |= (uint64_t)1 << (bit & 63);
        }
    } else {
        uint32_t bit, step, wmod;
        uint64_t acc = h1;
        ds_bloom_probe_init(b->nbits, h1, h2, &bit, &step, &wmod);
        for (i = 0; i < b->k; i++) {
            b->words[bit >> 6] |= (uint64_t)1 << (bit & 63);
            bit = ds_bloom_probe_next(b->nbits, &acc, h2, bit, step, wmod);
        }
    }
}

int dyn_bloom_may_contain(const dyn_bloom_t* b, const void* key, size_t n)
{
    uint64_t h1, h2;
    uint32_t i;
    ds_bloom_hashes((const char*)key, n, &h1, &h2);
    if (b->k <= 4) {
        for (i = 0; i < b->k; i++) {
            uint32_t bit = (uint32_t)((h1 + (uint64_t)i * h2) % b->nbits);
            if ((b->words[bit >> 6] & ((uint64_t)1 << (bit & 63))) == 0)
                return 0;
        }
    } else {
        uint32_t bit, step, wmod;
        uint64_t acc = h1;
        ds_bloom_probe_init(b->nbits, h1, h2, &bit, &step, &wmod);
        for (i = 0; i < b->k; i++) {
            if ((b->words[bit >> 6] & ((uint64_t)1 << (bit & 63))) == 0)
                return 0;
            bit = ds_bloom_probe_next(b->nbits, &acc, h2, bit, step, wmod);
        }
    }
    return 1;
}
