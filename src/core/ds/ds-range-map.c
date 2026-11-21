#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-range-map.h"

typedef struct {
    double lo, hi;
    dyn_cell_t val;
} ds_rmap_span;

struct dyn_rmap {
    ds_cvec g;
};

#define DS_RM(map, i) ((ds_rmap_span*)ds_cv_at(&(map)->g, sizeof(ds_rmap_span), (i)))

static uint32_t ds_rm_first_hi_above(const dyn_rmap_t* m, double x)
{
    return ds_cv_first_hi(&m->g, sizeof(ds_rmap_span), x, 1);
}

dyn_rmap_t* dyn_rmap_new(void)
{
    return (dyn_rmap_t*)ds_calloc(1, sizeof(dyn_rmap_t));
}

void dyn_rmap_free(dyn_rmap_t* m, dyn_cell_free_fn fn, void* ud)
{
    uint32_t i;
    if (!m)
        return;
    if (fn)
        for (i = 0; i < m->g.len; i++)
            fn(ud, &DS_RM(m, i)->val);
    ds_cv_free(&m->g);
    DYN_DS_FREE(m);
}

static int ds_rmap_carve(dyn_rmap_t* m, double lo, double hi,
    dyn_cell_dup_fn dup, dyn_cell_free_fn fn, void* ud,
    uint32_t* at)
{
    uint32_t i, j;

    i = ds_rm_first_hi_above(m, lo);
    j = i;
    while (j < m->g.len && DS_RM(m, j)->lo < hi)
        j++;
    *at = i;
    if (j == i)
        return 0;

    if (DS_RM(m, i)->lo < lo && DS_RM(m, j - 1)->hi > hi && j - i == 1) {
        ds_rmap_span right;
        right.lo = hi;
        right.hi = DS_RM(m, i)->hi;
        dup(ud, &DS_RM(m, i)->val, &right.val);
        if (ds_cv_splice(&m->g, sizeof(ds_rmap_span), i + 1, 0, &right, 1) < 0) {
            if (fn)
                fn(ud, &right.val);
            return -1;
        }
        DS_RM(m, i)->hi = lo;
        *at = i + 1;
        return 0;
    }
    if (DS_RM(m, i)->lo < lo) {
        DS_RM(m, i)->hi = lo;
        i++;
        *at = i;
    }
    if (j > i && DS_RM(m, j - 1)->hi > hi) {
        DS_RM(m, j - 1)->lo = hi;
        j--;
    }
    if (j > i) {
        uint32_t k;
        if (fn)
            for (k = i; k < j; k++)
                fn(ud, &DS_RM(m, k)->val);
        ds_cv_splice(&m->g, sizeof(ds_rmap_span), i, j - i, NULL, 0);
    }
    return 0;
}

int dyn_rmap_put(dyn_rmap_t* m, double lo, double hi, const dyn_cell_t* v,
    dyn_cell_dup_fn dup, dyn_cell_free_fn fn, void* ud)
{
    uint32_t at;
    ds_rmap_span ns;
    if (!(hi > lo))
        return 0;
    if (ds_rmap_carve(m, lo, hi, dup, fn, ud, &at) < 0)
        return -1;
    ns.lo = lo;
    ns.hi = hi;
    ns.val = *v;
    return ds_cv_splice(&m->g, sizeof(ds_rmap_span), at, 0, &ns, 1);
}

int dyn_rmap_remove(dyn_rmap_t* m, double lo, double hi,
    dyn_cell_dup_fn dup, dyn_cell_free_fn fn, void* ud)
{
    uint32_t at;
    if (!(hi > lo))
        return 0;
    return ds_rmap_carve(m, lo, hi, dup, fn, ud, &at);
}

const dyn_cell_t* dyn_rmap_get(const dyn_rmap_t* m, double x)
{
    uint32_t lo = ds_rm_first_hi_above(m, x);
    return (lo < m->g.len && DS_RM(m, lo)->lo <= x) ? &DS_RM(m, lo)->val : NULL;
}

uint32_t dyn_rmap_count(const dyn_rmap_t* m) { return m->g.len; }

int dyn_rmap_at(const dyn_rmap_t* m, uint32_t i, double* lo, double* hi,
    const dyn_cell_t** v)
{
    if (i >= m->g.len)
        return 0;
    *lo = DS_RM(m, i)->lo;
    *hi = DS_RM(m, i)->hi;
    *v = &DS_RM(m, i)->val;
    return 1;
}
