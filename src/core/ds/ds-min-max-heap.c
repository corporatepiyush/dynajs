#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-min-max-heap.h"

typedef struct {
    double pri;
    dyn_cell_t val;
} ds_mmnode;

struct dyn_mmheap {
    ds_mmnode* v;
    uint32_t len, cap;
};

dyn_mmheap_t* dyn_mmheap_new(void)
{
    return (dyn_mmheap_t*)ds_calloc(1, sizeof(dyn_mmheap_t));
}

void dyn_mmheap_free(dyn_mmheap_t* h, dyn_cell_free_fn fn, void* ud)
{
    uint32_t i;
    if (!h)
        return;
    if (fn)
        for (i = 0; i < h->len; i++)
            fn(ud, &h->v[i].val);
    DYN_DS_FREE(h->v);
    DYN_DS_FREE(h);
}

uint32_t dyn_mmheap_size(const dyn_mmheap_t* h) { return h->len; }

const dyn_cell_t* dyn_mmheap_cell_at(const dyn_mmheap_t* h, uint32_t i)
{
    return i < h->len ? &h->v[i].val : NULL;
}

double dyn_mmheap_pri_at(const dyn_mmheap_t* h, uint32_t i)
{
    return i < h->len ? h->v[i].pri : 0.0;
}

static int ds_mm_is_min_level(uint32_t i)
{
#if defined(__GNUC__) || defined(__clang__)
    return (31 - __builtin_clz(i + 1)) % 2 == 0;
#else
    int d = 0;
    uint32_t n = i + 1;
    while (n > 1) {
        n >>= 1;
        d++;
    }
    return (d & 1) == 0;
#endif
}

static void ds_mm_swap(ds_mmnode* a, ds_mmnode* b)
{
    ds_mmnode t = *a;
    *a = *b;
    *b = t;
}

static void ds_mm_bubble_up_dir(dyn_mmheap_t* h, uint32_t i, int want_max)
{
    while (i >= 3) {
        uint32_t g = (i - 3) / 4;
        int better = want_max ? (h->v[i].pri > h->v[g].pri)
                              : (h->v[i].pri < h->v[g].pri);
        if (!better)
            return;
        ds_mm_swap(&h->v[i], &h->v[g]);
        i = g;
    }
}

static void ds_mm_bubble_up(dyn_mmheap_t* h, uint32_t i)
{
    uint32_t p;
    if (i == 0)
        return;
    p = (i - 1) / 2;
    if (ds_mm_is_min_level(i)) {
        if (h->v[i].pri > h->v[p].pri) {
            ds_mm_swap(&h->v[i], &h->v[p]);
            ds_mm_bubble_up_dir(h, p, 1);
        } else {
            ds_mm_bubble_up_dir(h, i, 0);
        }
    } else {
        if (h->v[i].pri < h->v[p].pri) {
            ds_mm_swap(&h->v[i], &h->v[p]);
            ds_mm_bubble_up_dir(h, p, 0);
        } else {
            ds_mm_bubble_up_dir(h, i, 1);
        }
    }
}

static uint32_t ds_mm_best_descendant(dyn_mmheap_t* h, uint32_t i, int want_max,
    int* is_gc)
{
    uint32_t best = UINT32_MAX, c, g;
    for (c = 2 * i + 1; c <= 2 * i + 2 && c < h->len; c++) {
        if (best == UINT32_MAX || (want_max ? h->v[c].pri > h->v[best].pri : h->v[c].pri < h->v[best].pri)) {
            best = c;
            *is_gc = 0;
        }
        for (g = 2 * c + 1; g <= 2 * c + 2 && g < h->len; g++)
            if (want_max ? h->v[g].pri > h->v[best].pri
                         : h->v[g].pri < h->v[best].pri) {
                best = g;
                *is_gc = 1;
            }
    }
    return best;
}

static void ds_mm_trickle_down_dir(dyn_mmheap_t* h, uint32_t i, int want_max)
{
    for (;;) {
        int is_gc = 0;
        uint32_t m = ds_mm_best_descendant(h, i, want_max, &is_gc);
        if (m == UINT32_MAX)
            return;
        if (!is_gc) {
            if (want_max ? h->v[m].pri > h->v[i].pri
                         : h->v[m].pri < h->v[i].pri)
                ds_mm_swap(&h->v[m], &h->v[i]);
            return;
        }
        if (!(want_max ? h->v[m].pri > h->v[i].pri
                       : h->v[m].pri < h->v[i].pri))
            return;
        ds_mm_swap(&h->v[m], &h->v[i]);
        {
            uint32_t p = (m - 1) / 2;
            if (want_max ? h->v[m].pri < h->v[p].pri
                         : h->v[m].pri > h->v[p].pri)
                ds_mm_swap(&h->v[m], &h->v[p]);
        }
        i = m;
    }
}

static void ds_mm_trickle_down(dyn_mmheap_t* h, uint32_t i)
{
    ds_mm_trickle_down_dir(h, i, !ds_mm_is_min_level(i));
}

int dyn_mmheap_push(dyn_mmheap_t* h, double pri, const dyn_cell_t* v)
{
    if (ds_reserve((void**)&h->v, &h->cap, h->len + 1, sizeof(*h->v)) < 0)
        return -1;
    h->v[h->len].pri = pri;
    h->v[h->len].val = *v;
    h->len++;
    ds_mm_bubble_up(h, h->len - 1);
    return 0;
}

static uint32_t ds_mm_max_index(const dyn_mmheap_t* h)
{
    if (h->len <= 1)
        return 0;
    if (h->len == 2)
        return 1;
    return h->v[1].pri >= h->v[2].pri ? 1 : 2;
}

static int ds_mm_pop_at(dyn_mmheap_t* h, uint32_t i, double* pri,
    dyn_cell_t* out)
{
    if (h->len == 0)
        return 0;
    *pri = h->v[i].pri;
    *out = h->v[i].val;
    h->len--;
    if (i != h->len) {
        h->v[i] = h->v[h->len];
        ds_mm_trickle_down(h, i);
    }
    return 1;
}

int dyn_mmheap_pop_min(dyn_mmheap_t* h, double* pri, dyn_cell_t* out)
{
    return ds_mm_pop_at(h, 0, pri, out);
}

int dyn_mmheap_pop_max(dyn_mmheap_t* h, double* pri, dyn_cell_t* out)
{
    return ds_mm_pop_at(h, ds_mm_max_index(h), pri, out);
}

int dyn_mmheap_peek_min(const dyn_mmheap_t* h, double* pri,
    const dyn_cell_t** v)
{
    if (h->len == 0)
        return 0;
    *pri = h->v[0].pri;
    *v = &h->v[0].val;
    return 1;
}

int dyn_mmheap_peek_max(const dyn_mmheap_t* h, double* pri,
    const dyn_cell_t** v)
{
    uint32_t i;
    if (h->len == 0)
        return 0;
    i = ds_mm_max_index(h);
    *pri = h->v[i].pri;
    *v = &h->v[i].val;
    return 1;
}
