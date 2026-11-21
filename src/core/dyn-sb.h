#ifndef DYNA_CORE_SB_H
#define DYNA_CORE_SB_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static inline size_t dyn_sb_grow_cap(size_t cur, size_t need, size_t seed)
{
    size_t nc = cur ? cur : seed;
    while (nc < need) {
        if (nc < (1u << 16))
            nc *= 2;
        else if (nc < (1u << 20))
            nc += nc / 2;
        else
            nc += nc / 4;
    }
    return nc;
}

static inline int dyn_sb_reserve(void** pp, size_t* pcap, size_t need,
    size_t seed)
{
    size_t nc;
    void* np;

    if (need <= *pcap)
        return 1;
    nc = dyn_sb_grow_cap(*pcap, need, seed);
    np = realloc(*pp, nc);
    if (!np)
        return 0;
    *pp = np;
    *pcap = nc;
    return 1;
}

typedef struct {
    char* p;
    size_t n, cap;
    int oom;
} dyn_sb_t;

static inline void dyn_sb_init(dyn_sb_t* b)
{
    b->p = NULL;
    b->n = 0;
    b->cap = 0;
    b->oom = 0;
}

static inline void dyn_sb_free(dyn_sb_t* b)
{
    free(b->p);
    b->p = NULL;
    b->n = 0;
    b->cap = 0;
}

static inline void dyn_sb_put(dyn_sb_t* b, const char* s, size_t n)
{
    if (b->oom || n == 0)
        return;
    if (b->n + n > b->cap
        && !dyn_sb_reserve((void**)&b->p, &b->cap, b->n + n, 64)) {
        b->oom = 1;
        return;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

static inline void dyn_sb_putc(dyn_sb_t* b, char c) { dyn_sb_put(b, &c, 1); }

static inline void dyn_sb_puts(dyn_sb_t* b, const char* s) { dyn_sb_put(b, s, strlen(s)); }

#endif
