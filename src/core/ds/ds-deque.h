#ifndef DS_DEQUE_H
#define DS_DEQUE_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"
#include "ds-cell.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    dyn_cell_t* buf;
    uint32_t cap;
    uint32_t head;
    uint32_t count;
    dyn_cell_free_fn release;
    void* ud;
} dyn_deque_t;

void dyn_deque_init(dyn_deque_t* d, dyn_cell_free_fn release, void* ud);

void dyn_deque_free(dyn_deque_t* d);

int dyn_deque_grow(dyn_deque_t* d);

void dyn_deque_shrink(dyn_deque_t* d);

static inline dyn_cell_t* dyn_deque_at(const dyn_deque_t* d, uint32_t i)
{
    if (i >= d->count)
        return NULL;
    return &d->buf[(d->head + i) & (d->cap - 1)];
}

static inline int dyn_deque_push_back(dyn_deque_t* d, const dyn_cell_t* v)
{
    if ((d->count < d->cap ? 0 : dyn_deque_grow(d)) < 0)
        return -1;
    d->buf[(d->head + d->count) & (d->cap - 1)] = *v;
    d->count++;
    return 0;
}

static inline int dyn_deque_push_front(dyn_deque_t* d, const dyn_cell_t* v)
{
    if ((d->count < d->cap ? 0 : dyn_deque_grow(d)) < 0)
        return -1;
    d->head = (d->head - 1) & (d->cap - 1);
    d->buf[d->head] = *v;
    d->count++;
    return 0;
}

static inline int dyn_deque_pop_front(dyn_deque_t* d, dyn_cell_t* out)
{
    if (d->count == 0)
        return -1;
    *out = d->buf[d->head];
    d->head = (d->head + 1) & (d->cap - 1);
    d->count--;
    if (d->cap >= 64 && d->count <= d->cap / 4)
        dyn_deque_shrink(d);
    return 0;
}

static inline int dyn_deque_pop_back(dyn_deque_t* d, dyn_cell_t* out)
{
    if (d->count == 0)
        return -1;
    d->count--;
    *out = d->buf[(d->head + d->count) & (d->cap - 1)];
    if (d->cap >= 64 && d->count <= d->cap / 4)
        dyn_deque_shrink(d);
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif
