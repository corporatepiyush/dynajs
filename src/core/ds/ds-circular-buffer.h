#ifndef DS_CIRCULAR_BUFFER_H
#define DS_CIRCULAR_BUFFER_H

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
} dyn_circbuf_t;

int dyn_circbuf_init(dyn_circbuf_t* c, uint32_t cap, dyn_cell_free_fn release, void* ud);

void dyn_circbuf_free(dyn_circbuf_t* c);

static inline dyn_cell_t* dyn_circbuf_at(const dyn_circbuf_t* c, uint32_t i)
{
    uint32_t idx;
    if (i >= c->count)
        return NULL;
    idx = c->head + i;
    return &c->buf[idx >= c->cap ? idx - c->cap : idx];
}

static inline int dyn_circbuf_push(dyn_circbuf_t* c, const dyn_cell_t* v, dyn_cell_t* evicted)
{
    if (c->count < c->cap) {
        uint32_t idx = c->head + c->count;
        c->buf[idx >= c->cap ? idx - c->cap : idx] = *v;
        c->count++;
        return 0;
    }
    if (evicted)
        *evicted = c->buf[c->head];
    else if (c->release)
        c->release(c->ud, &c->buf[c->head]);
    c->buf[c->head] = *v;
    if (++c->head == c->cap)
        c->head = 0;
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif
