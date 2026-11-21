#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-interval-tree.h"

typedef struct {
    double lo, hi;
    dyn_cell_t val;
} ds_ival;

struct dyn_itree {
    ds_ival* v;
    uint32_t len, cap;
    double* mx;
    uint32_t sz;
    uint32_t mxcap;
    uint32_t built;
    int dirty;
    uint32_t qsince;
};

#define DS_ITREE_PENDING_MAX(built) (16u + (built) / 8u)
#define DS_ITREE_BURST_MIN 1024u

dyn_itree_t* dyn_itree_new(void)
{
    return (dyn_itree_t*)ds_calloc(1, sizeof(dyn_itree_t));
}

void dyn_itree_free(dyn_itree_t* t, dyn_cell_free_fn fn, void* ud)
{
    uint32_t i;
    if (!t)
        return;
    if (fn)
        for (i = 0; i < t->len; i++)
            fn(ud, &t->v[i].val);
    DYN_DS_FREE(t->v);
    DYN_DS_FREE(t->mx);
    DYN_DS_FREE(t);
}

int dyn_itree_insert(dyn_itree_t* t, double lo, double hi,
    const dyn_cell_t* v)
{
    if (ds_reserve((void**)&t->v, &t->cap, t->len + 1, sizeof(*t->v)) < 0)
        return -1;
    t->v[t->len].lo = lo;
    t->v[t->len].hi = hi < lo ? lo : hi;
    t->v[t->len].val = *v;
    t->len++;
    t->dirty = 1;
    t->qsince = 0;
    return 0;
}

uint32_t dyn_itree_size(const dyn_itree_t* t) { return t->len; }

static int ds_ival_cmp(const void* a, const void* b)
{
    double x = ((const ds_ival*)a)->lo, y = ((const ds_ival*)b)->lo;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int ds_itree_build(dyn_itree_t* t)
{
    uint32_t sz = 1, i;
    if (!t->dirty)
        return 0;
    if (t->len == 0) {
        t->dirty = 0;
        t->sz = 0;
        t->built = 0;
        t->qsince = 0;
        return 0;
    }
    t->qsince++;
    if (t->built && (t->qsince < 2 || t->len - t->built < DS_ITREE_BURST_MIN) && t->len - t->built <= DS_ITREE_PENDING_MAX(t->built))
        return 0;
    qsort(t->v, t->len, sizeof(*t->v), ds_ival_cmp);
    while (sz < t->len)
        sz *= 2;
    if (ds_reserve((void**)&t->mx, &t->mxcap, 2 * sz, sizeof(*t->mx)) < 0)
        return -1;
    for (i = 0; i < sz; i++)
        t->mx[sz + i] = i < t->len ? t->v[i].hi : -(double)INFINITY;
    for (i = sz - 1; i >= 1; i--)
        t->mx[i] = t->mx[2 * i] > t->mx[2 * i + 1] ? t->mx[2 * i]
                                                   : t->mx[2 * i + 1];
    t->sz = sz;
    t->built = t->len;
    t->dirty = 0;
    t->qsince = 0;
    return 0;
}

uint32_t dyn_itree_query(dyn_itree_t* t, double lo, double hi,
    uint32_t* out, uint32_t cap)
{
    uint32_t stack[128], top = 0, found = 0, k, p;

    if (ds_itree_build(t) < 0 || t->len == 0)
        return 0;
    for (p = t->built; p < t->len; p++)
        if (t->v[p].lo <= hi && t->v[p].hi >= lo) {
            if (found < cap)
                out[found] = p;
            found++;
        }
    if (t->built == 0)
        return found;
    {
        uint32_t a = 0, b = t->built;
        while (a < b) {
            uint32_t mid = a + (b - a) / 2;
            if (t->v[mid].lo <= hi)
                a = mid + 1;
            else
                b = mid;
        }
        k = a;
    }
    if (k == 0)
        return found;

    stack[top++] = 1;
    while (top) {
        uint32_t n = stack[--top], lvl = 0, m = n, base, width;
        while (m > 1) {
            m >>= 1;
            lvl++;
        }
        width = t->sz >> lvl;
        base = (n - (1u << lvl)) * width;
        if (base >= k || t->mx[n] < lo)
            continue;
        if (width == 1) {
            if (found < cap)
                out[found] = base;
            found++;
            continue;
        }
        stack[top++] = 2 * n + 1;
        stack[top++] = 2 * n;
    }
    return found;
}

int dyn_itree_at(const dyn_itree_t* t, uint32_t i, double* lo, double* hi,
    const dyn_cell_t** v)
{
    if (i >= t->len)
        return 0;
    *lo = t->v[i].lo;
    *hi = t->v[i].hi;
    *v = &t->v[i].val;
    return 1;
}

int dyn_itree_remove_at(dyn_itree_t* t, uint32_t i, dyn_cell_t* out)
{
    if (i >= t->len)
        return 0;
    *out = t->v[i].val;
    t->len--;
    if (i != t->len)
        t->v[i] = t->v[t->len];
    t->built = 0;
    t->dirty = 1;
    return 1;
}
