#ifndef DYNA_CORE_SB_H
#define DYNA_CORE_SB_H

#include <stddef.h>
#include <stdlib.h>

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

#endif
