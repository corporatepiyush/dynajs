#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-mpmc-ring.h"

#ifndef DYN_DS_NO_THREADS
struct dyn_ring {
    size_t esz, stride, mask;
    uint8_t* buf;
    char pad0[64];
    atomic_size_t enq;
    char pad1[64];
    atomic_size_t deq;
    char pad2[64];
};

#define DS_RING_SEQ(r, i) ((atomic_size_t*)(void*)((r)->buf + ((i) & (r)->mask) * (r)->stride))
#define DS_RING_ELEM(r, i) ((void*)((r)->buf + ((i) & (r)->mask) * (r)->stride + sizeof(atomic_size_t)))

dyn_ring_t* dyn_ring_new(size_t elem_size, size_t capacity)
{
    dyn_ring_t* r;
    size_t cap = 2, i;
    if (!elem_size || capacity > ((size_t)1 << 40))
        return NULL;
    while (cap < capacity)
        cap <<= 1;
    r = (dyn_ring_t*)DYN_DS_CALLOC(1, sizeof(*r));
    if (!r)
        return NULL;
    r->esz = elem_size;
    r->stride = ds_align8(sizeof(atomic_size_t) + elem_size);
    r->mask = cap - 1;
    r->buf = (uint8_t*)DYN_DS_MALLOC(cap * r->stride);
    if (!r->buf) {
        DYN_DS_FREE(r);
        return NULL;
    }
    for (i = 0; i < cap; i++)
        atomic_init(DS_RING_SEQ(r, i), i);
    atomic_init(&r->enq, 0);
    atomic_init(&r->deq, 0);
    return r;
}

void dyn_ring_free(dyn_ring_t* r)
{
    if (!r)
        return;
    DYN_DS_FREE(r->buf);
    DYN_DS_FREE(r);
}

size_t dyn_ring_capacity(const dyn_ring_t* r)
{
    return r->mask + 1;
}

int dyn_ring_try_push(dyn_ring_t* r, const void* elem)
{
    size_t pos = atomic_load_explicit(&r->enq, memory_order_relaxed);
    for (;;) {
        atomic_size_t* seq = DS_RING_SEQ(r, pos);
        size_t s = atomic_load_explicit(seq, memory_order_acquire);
        intptr_t dif = (intptr_t)s - (intptr_t)pos;
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(&r->enq, &pos, pos + 1, memory_order_relaxed, memory_order_relaxed)) {
                memcpy(DS_RING_ELEM(r, pos), elem, r->esz);
                atomic_store_explicit(seq, pos + 1, memory_order_release);
                return 0;
            }
        } else if (dif < 0) {
            return -1;
        } else {
            pos = atomic_load_explicit(&r->enq, memory_order_relaxed);
        }
    }
}

int dyn_ring_try_pop(dyn_ring_t* r, void* out)
{
    size_t pos = atomic_load_explicit(&r->deq, memory_order_relaxed);
    for (;;) {
        atomic_size_t* seq = DS_RING_SEQ(r, pos);
        size_t s = atomic_load_explicit(seq, memory_order_acquire);
        intptr_t dif = (intptr_t)s - (intptr_t)(pos + 1);
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(&r->deq, &pos, pos + 1, memory_order_relaxed, memory_order_relaxed)) {
                memcpy(out, DS_RING_ELEM(r, pos), r->esz);
                atomic_store_explicit(seq, pos + r->mask + 1, memory_order_release);
                return 0;
            }
        } else if (dif < 0) {
            return -1;
        } else {
            pos = atomic_load_explicit(&r->deq, memory_order_relaxed);
        }
    }
}

size_t dyn_ring_size_approx(const dyn_ring_t* r)
{
    size_t e = atomic_load_explicit(&r->enq, memory_order_relaxed);
    size_t d = atomic_load_explicit(&r->deq, memory_order_relaxed);
    return e > d ? e - d : 0;
}
#endif
