#include "ds-internal.h"
#include "ds-heap.h"

void ds_heap_push(ds_heap_item* h, size_t* n, ds_heap_item it)
{
    size_t i = (*n)++;
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (h[p].d <= it.d)
            break;
        h[i] = h[p];
        i = p;
    }
    h[i] = it;
}

ds_heap_item ds_heap_pop(ds_heap_item* h, size_t* n)
{
    ds_heap_item top = h[0], last = h[--(*n)];
    size_t i = 0;
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= *n)
            break;
        if (c + 1 < *n && h[c + 1].d < h[c].d)
            c++;
        if (last.d <= h[c].d)
            break;
        h[i] = h[c];
        i = c;
    }
    if (*n)
        h[i] = last;
    return top;
}

void dyn_heap_init(dyn_heap_t* h)
{
    h->items = NULL;
    h->count = 0;
    h->cap = 0;
}

void dyn_heap_free(dyn_heap_t* h, dyn_cell_free_fn release, void* ud)
{
    uint32_t i;
    if (release)
        for (i = 0; i < h->count; i++)
            release(ud, &h->items[i]);
    DYN_DS_FREE(h->items);
    dyn_heap_init(h);
}

int dyn_heap_reserve(dyn_heap_t* h, uint32_t need)
{
    uint32_t ncap;
    dyn_cell_t* ni;
    if (need <= h->cap)
        return 0;
    if (need > UINT32_MAX / (uint32_t)sizeof(dyn_cell_t))
        return -1;
    ncap = h->cap ? h->cap : 8;
    while (ncap < need) {
        if (ncap > UINT32_MAX / 2 || ncap > UINT32_MAX / (uint32_t)sizeof(dyn_cell_t) / 2) {
            ncap = need;
            break;
        }
        ncap *= 2;
    }
    ni = (dyn_cell_t*)DYN_DS_REALLOC(h->items, (size_t)ncap * sizeof(dyn_cell_t));
    if (!ni)
        return -1;
    h->items = ni;
    h->cap = ncap;
    return 0;
}
