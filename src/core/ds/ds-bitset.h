#ifndef DS_BITSET_H
#define DS_BITSET_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t* words;
    uint32_t nwords;
    uint32_t cc;
} dyn_bitset_t;

enum { DYN_BITSET_AND,
    DYN_BITSET_OR,
    DYN_BITSET_XOR };

void dyn_bitset_init(dyn_bitset_t* b);

void dyn_bitset_free(dyn_bitset_t* b);

int dyn_bitset_reserve(dyn_bitset_t* b, uint32_t word_index);

uint64_t dyn_bitset_count(const dyn_bitset_t* b);

int64_t dyn_bitset_next_set(const dyn_bitset_t* b, uint32_t from);

int dyn_bitset_combine(dyn_bitset_t* a, const dyn_bitset_t* o, int op);

static inline int dyn_bitset_set(dyn_bitset_t* b, uint32_t i)
{
    if ((i >> 6) >= b->nwords && dyn_bitset_reserve(b, i >> 6) < 0)
        return -1;
    b->words[i >> 6] |= (uint64_t)1 << (i & 63);
    return 0;
}

static inline int dyn_bitset_flip(dyn_bitset_t* b, uint32_t i)
{
    if ((i >> 6) >= b->nwords && dyn_bitset_reserve(b, i >> 6) < 0)
        return -1;
    b->words[i >> 6] ^= (uint64_t)1 << (i & 63);
    return 0;
}

static inline void dyn_bitset_clear(dyn_bitset_t* b, uint32_t i)
{
    if ((i >> 6) < b->nwords)
        b->words[i >> 6] &= ~((uint64_t)1 << (i & 63));
}

static inline int dyn_bitset_get(const dyn_bitset_t* b, uint32_t i)
{
    return (i >> 6) < b->nwords && (b->words[i >> 6] & ((uint64_t)1 << (i & 63))) != 0;
}

#ifdef __cplusplus
}
#endif

#endif
