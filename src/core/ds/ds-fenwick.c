#include "ds-internal.h"
#include "ds-fenwick.h"

int dyn_fenwick_init(dyn_fenwick_t* f, uint32_t n)
{
    f->n = n;
    f->tree = (double*)DYN_DS_CALLOC((size_t)n + 1, sizeof(double));
    return f->tree ? 0 : -1;
}

void dyn_fenwick_free(dyn_fenwick_t* f)
{
    DYN_DS_FREE(f->tree);
    f->tree = NULL;
    f->n = 0;
}

double dyn_fenwick_range(const dyn_fenwick_t* f, uint32_t lo, uint32_t hi)
{
    uint32_t idx;
    double s = 0.0;
    if (lo > hi)
        return 0.0;
    for (idx = hi + 1; idx > 0; idx -= idx & (~idx + 1))
        s += f->tree[idx];
    for (idx = lo; idx > 0; idx -= idx & (~idx + 1))
        s -= f->tree[idx];
    return s;
}
