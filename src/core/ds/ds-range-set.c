#include "ds-internal.h"
#include "ds-heap.h"

#define DS_RS(set, i) ((ds_span*)ds_cv_at(&(set)->g, sizeof(ds_span), (i)))

dyn_rset_t* dyn_rset_new(void)
{
    return (dyn_rset_t*)ds_calloc(1, sizeof(dyn_rset_t));
}

void dyn_rset_free(dyn_rset_t* s)
{
    if (!s)
        return;
    ds_cv_free(&s->g);
    DYN_DS_FREE(s);
}

void dyn_rset_clear(dyn_rset_t* s)
{
    ds_cv_clear(&s->g);
}

static uint32_t ds_rset_lower(const dyn_rset_t* s, double x)
{
    return ds_rs_first_hi(s, x, 1);
}

int dyn_rset_add(dyn_rset_t* s, double lo, double hi)
{
    uint32_t i, j;
    ds_span merged;

    if (!(hi > lo))
        return 0;
    i = ds_rs_first_hi(s, lo, 0);
    j = i;
    merged.lo = lo;
    merged.hi = hi;
    while (j < s->g.len && DS_RS(s, j)->lo <= merged.hi) {
        const ds_span* c = DS_RS(s, j);
        if (c->lo < merged.lo)
            merged.lo = c->lo;
        if (c->hi > merged.hi)
            merged.hi = c->hi;
        j++;
    }
    return ds_cv_splice(&s->g, sizeof(ds_span), i, j - i, &merged, 1);
}

int dyn_rset_remove(dyn_rset_t* s, double lo, double hi)
{
    uint32_t i, j, n = 0;
    ds_span keep[2];

    if (!(hi > lo))
        return 0;
    i = ds_rset_lower(s, lo);
    j = i;
    while (j < s->g.len && DS_RS(s, j)->lo < hi)
        j++;
    if (j == i)
        return 0;
    if (DS_RS(s, i)->lo < lo) {
        keep[n].lo = DS_RS(s, i)->lo;
        keep[n].hi = lo;
        n++;
    }
    if (DS_RS(s, j - 1)->hi > hi) {
        keep[n].lo = hi;
        keep[n].hi = DS_RS(s, j - 1)->hi;
        n++;
    }
    return ds_cv_splice(&s->g, sizeof(ds_span), i, j - i, keep, n);
}

int dyn_rset_contains(const dyn_rset_t* s, double x)
{
    uint32_t i = ds_rset_lower(s, x);
    return i < s->g.len && DS_RS(s, i)->lo <= x;
}

int dyn_rset_encloses(const dyn_rset_t* s, double lo, double hi)
{
    uint32_t i;
    if (!(hi > lo))
        return 1;
    i = ds_rset_lower(s, lo);
    return i < s->g.len && DS_RS(s, i)->lo <= lo && DS_RS(s, i)->hi >= hi;
}

int dyn_rset_intersects(const dyn_rset_t* s, double lo, double hi)
{
    uint32_t i;
    if (!(hi > lo))
        return 0;
    i = ds_rset_lower(s, lo);
    return i < s->g.len && DS_RS(s, i)->lo < hi;
}

uint32_t dyn_rset_count(const dyn_rset_t* s) { return s->g.len; }

int dyn_rset_at(const dyn_rset_t* s, uint32_t i, double* lo, double* hi)
{
    if (i >= s->g.len)
        return 0;
    *lo = DS_RS(s, i)->lo;
    *hi = DS_RS(s, i)->hi;
    return 1;
}

int dyn_rset_complement(const dyn_rset_t* s, double lo, double hi,
    dyn_rset_t* out)
{
    double cur = lo;
    uint32_t i;

    dyn_rset_clear(out);
    if (!(hi > lo))
        return 0;
    for (i = 0; i < s->g.len; i++) {
        const ds_span* c = DS_RS(s, i);
        if (c->hi <= cur)
            continue;
        if (c->lo >= hi)
            break;
        if (c->lo > cur && dyn_rset_add(out, cur, c->lo) < 0)
            return -1;
        if (c->hi > cur)
            cur = c->hi;
        if (cur >= hi)
            break;
    }
    if (cur < hi && dyn_rset_add(out, cur, hi) < 0)
        return -1;
    return 0;
}
