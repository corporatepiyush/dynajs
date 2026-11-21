#include "ds-internal.h"
#include "ds-heap.h"

void dyn_bitset_init(dyn_bitset_t* b)
{
    b->words = NULL;
    b->nwords = 0;
    b->cc = 0;
}

void dyn_bitset_free(dyn_bitset_t* b)
{
    DYN_DS_FREE(b->words);
    b->words = NULL;
    b->nwords = 0;
}

int dyn_bitset_reserve(dyn_bitset_t* b, uint32_t wi)
{
    uint32_t ncap;
    uint64_t* nw;
    if (wi < b->nwords)
        return 0;
    ncap = b->nwords ? b->nwords : 4;
    while (ncap <= wi) {
        if (ncap > UINT32_MAX / 2)
            return -1;
        ncap *= 2;
    }
    nw = (uint64_t*)DYN_DS_REALLOC(b->words, (size_t)ncap * sizeof(uint64_t));
    if (!nw)
        return -1;
    memset(nw + b->nwords, 0, (size_t)(ncap - b->nwords) * sizeof(uint64_t));
    b->words = nw;
    b->nwords = ncap;
    return 0;
}

uint64_t dyn_bitset_count(const dyn_bitset_t* b)
{
    uint64_t total = 0;
    uint32_t i;
    for (i = 0; i < b->nwords; i++)
        total += (uint64_t)__builtin_popcountll(b->words[i]);
    return total;
}

int64_t dyn_bitset_next_set(const dyn_bitset_t* b, uint32_t from)
{
    uint32_t wi = from >> 6;
    uint64_t w;
    if (wi >= b->nwords)
        return -1;
    w = b->words[wi] & (~(uint64_t)0 << (from & 63));
    for (;;) {
        if (w)
            return (int64_t)wi * 64 + __builtin_ctzll(w);
        if (++wi >= b->nwords)
            return -1;
        w = b->words[wi];
    }
}

int dyn_bitset_combine(dyn_bitset_t* a, const dyn_bitset_t* o, int op)
{
    uint32_t i;
    if (op != DYN_BITSET_AND && o->nwords > a->nwords && dyn_bitset_reserve(a, o->nwords - 1) < 0)
        return -1;
    for (i = 0; i < a->nwords; i++) {
        uint64_t ow = i < o->nwords ? o->words[i] : 0;
        if (op == DYN_BITSET_AND)
            a->words[i] &= ow;
        else if (op == DYN_BITSET_OR)
            a->words[i] |= ow;
        else
            a->words[i] ^= ow;
    }
    return 0;
}
