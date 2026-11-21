#ifndef DYN_GRAPHK_INT_H
#define DYN_GRAPHK_INT_H

#include <stdatomic.h>

#include "../dyn-par.h"
#include "graphk.h"

typedef struct {
    uint32_t* src;
    uint32_t* dst;
    double* w;
    uint64_t m;
} dyn_gk_arcs_t;

int dyn_gk_arcs_alloc(dyn_gk_arcs_t* a, uint64_t m, int with_w);
void dyn_gk_arcs_free(dyn_gk_arcs_t* a);
int dyn_gk_sort_merge(uint32_t n, const dyn_gk_arcs_t* a, int merge_min, dyn_gk_csr_t* out);

static inline uint32_t dyn_gk_par_lower(const uint64_t* cost, uint32_t nch, uint64_t target)
{
    uint32_t lo = 0, hi = nch;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (cost[mid] < target)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static inline void dyn_gk_par_span(const uint64_t* cost, uint32_t nch, unsigned w, unsigned nw, uint32_t* lo, uint32_t* hi)
{
    uint64_t tot = cost[nch];
    *lo = w == 0 ? 0 : dyn_gk_par_lower(cost, nch, tot * w / nw);
    *hi = w + 1 >= nw ? nch : dyn_gk_par_lower(cost, nch, tot * (w + 1) / nw);
    if (*hi < *lo)
        *hi = *lo;
}

static inline double dyn_gk_wt(const dyn_gk_csr_t* c, uint32_t e)
{
    return c->w ? c->w[e] : 1.0;
}

static inline int dyn_gk_stopped(const dyn_gk_ctl_t* ctl)
{
    return ctl && ctl->stop && ctl->stop(ctl->ud);
}

#endif
