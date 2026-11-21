#include "ds-internal.h"
#include "ds-deque.h"

void dyn_deque_init(dyn_deque_t* d, dyn_cell_free_fn release, void* ud)
{
    d->buf = NULL;
    d->cap = d->head = d->count = 0;
    d->release = release;
    d->ud = ud;
}

void dyn_deque_free(dyn_deque_t* d)
{
    uint32_t i;
    if (d->release)
        for (i = 0; i < d->count; i++)
            d->release(d->ud, dyn_deque_at(d, i));
    DYN_DS_FREE(d->buf);
    d->buf = NULL;
    d->cap = d->head = d->count = 0;
}

static int ds_deque_resize(dyn_deque_t* d, uint32_t ncap)
{
    dyn_cell_t* nb = (dyn_cell_t*)DYN_DS_MALLOC((size_t)ncap * sizeof(dyn_cell_t));
    if (!nb)
        return -1;
    if (d->count) {
        uint32_t first = d->cap - d->head;
        if (first > d->count)
            first = d->count;
        memcpy(nb, d->buf + d->head, (size_t)first * sizeof(dyn_cell_t));
        if (first < d->count)
            memcpy(nb + first, d->buf, (size_t)(d->count - first) * sizeof(dyn_cell_t));
    }
    DYN_DS_FREE(d->buf);
    d->buf = nb;
    d->cap = ncap;
    d->head = 0;
    return 0;
}

int dyn_deque_grow(dyn_deque_t* d)
{
    if (d->count < d->cap)
        return 0;
    if (d->cap > UINT32_MAX / 2 || d->cap > UINT32_MAX / (uint32_t)sizeof(dyn_cell_t) / 2)
        return -1;
    return ds_deque_resize(d, d->cap ? d->cap * 2 : 8);
}

void dyn_deque_shrink(dyn_deque_t* d)
{
    if (d->cap < 64 || d->count > d->cap / 4)
        return;
    (void)ds_deque_resize(d, d->cap / 2);
}
