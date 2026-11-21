#include "ds-internal.h"
#include "ds-circular-buffer.h"

int dyn_circbuf_init(dyn_circbuf_t* c, uint32_t cap, dyn_cell_free_fn release, void* ud)
{
    c->buf = (dyn_cell_t*)DYN_DS_MALLOC((size_t)(cap ? cap : 1) * sizeof(dyn_cell_t));
    c->cap = cap;
    c->head = c->count = 0;
    c->release = release;
    c->ud = ud;
    return c->buf ? 0 : -1;
}

void dyn_circbuf_free(dyn_circbuf_t* c)
{
    uint32_t i;
    if (c->release)
        for (i = 0; i < c->count; i++)
            c->release(c->ud, dyn_circbuf_at(c, i));
    DYN_DS_FREE(c->buf);
    c->buf = NULL;
    c->count = 0;
}
