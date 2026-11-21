#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-work-stealing-deque.h"

#ifndef DYN_DS_NO_THREADS
typedef struct ds_wsarray {
    int64_t size;
    struct ds_wsarray* retired;
    _Atomic uint64_t buf[];
} ds_wsarray;

struct dyn_wsdeque {
    char pad0[64];
    _Atomic int64_t top;
    char pad1[64];
    _Atomic int64_t bottom;
    char pad2[64];
    _Atomic(ds_wsarray*) array;
    ds_wsarray* retired;
};

static ds_wsarray* ds_ws_array_new(int64_t size)
{
    ds_wsarray* a = (ds_wsarray*)DYN_DS_MALLOC(sizeof(ds_wsarray) + sizeof(_Atomic uint64_t) * (size_t)size);
    int64_t i;
    if (!a)
        return NULL;
    a->size = size;
    a->retired = NULL;
    for (i = 0; i < size; i++)
        atomic_init(&a->buf[i], 0);
    return a;
}

dyn_wsdeque_t* dyn_wsdeque_new(size_t initial_capacity)
{
    dyn_wsdeque_t* q;
    int64_t size = 16;
    ds_wsarray* a;
    while ((size_t)size < initial_capacity && size < ((int64_t)1 << 40))
        size <<= 1;
    q = (dyn_wsdeque_t*)DYN_DS_CALLOC(1, sizeof(*q));
    if (!q)
        return NULL;
    a = ds_ws_array_new(size);
    if (!a) {
        DYN_DS_FREE(q);
        return NULL;
    }
    atomic_init(&q->top, 0);
    atomic_init(&q->bottom, 0);
    atomic_init(&q->array, a);
    return q;
}

void dyn_wsdeque_free(dyn_wsdeque_t* q)
{
    ds_wsarray *a, *r;
    if (!q)
        return;
    a = atomic_load_explicit(&q->array, memory_order_relaxed);
    DYN_DS_FREE(a);
    r = q->retired;
    while (r) {
        ds_wsarray* nx = r->retired;
        DYN_DS_FREE(r);
        r = nx;
    }
    DYN_DS_FREE(q);
}

int dyn_wsdeque_push(dyn_wsdeque_t* q, uint64_t item)
{
    int64_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed);
    int64_t t = atomic_load_explicit(&q->top, memory_order_acquire);
    ds_wsarray* a = atomic_load_explicit(&q->array, memory_order_relaxed);
    if (b - t > a->size - 1) {
        ds_wsarray* na = ds_ws_array_new(a->size * 2);
        int64_t i;
        if (!na)
            return -1;
        for (i = t; i < b; i++)
            atomic_store_explicit(&na->buf[i & (na->size - 1)], atomic_load_explicit(&a->buf[i & (a->size - 1)], memory_order_relaxed), memory_order_relaxed);
        a->retired = q->retired;
        q->retired = a;
        atomic_store_explicit(&q->array, na, memory_order_release);
        a = na;
    }
    atomic_store_explicit(&a->buf[b & (a->size - 1)], item, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&q->bottom, b + 1, memory_order_relaxed);
    return 0;
}

int dyn_wsdeque_pop(dyn_wsdeque_t* q, uint64_t* item)
{
    int64_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed) - 1;
    ds_wsarray* a = atomic_load_explicit(&q->array, memory_order_relaxed);
    int64_t t;
    int ok = 0;
    atomic_store_explicit(&q->bottom, b, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    t = atomic_load_explicit(&q->top, memory_order_relaxed);
    if (t <= b) {
        *item = atomic_load_explicit(&a->buf[b & (a->size - 1)], memory_order_relaxed);
        ok = 1;
        if (t == b) {
            if (!atomic_compare_exchange_strong_explicit(&q->top, &t, t + 1, memory_order_seq_cst, memory_order_relaxed))
                ok = 0;
            atomic_store_explicit(&q->bottom, b + 1, memory_order_relaxed);
        }
    } else {
        atomic_store_explicit(&q->bottom, b + 1, memory_order_relaxed);
    }
    return ok ? 0 : -1;
}

int dyn_wsdeque_steal(dyn_wsdeque_t* q, uint64_t* item)
{
    int64_t t = atomic_load_explicit(&q->top, memory_order_acquire);
    int64_t b;
    atomic_thread_fence(memory_order_seq_cst);
    b = atomic_load_explicit(&q->bottom, memory_order_acquire);
    if (t < b) {
        ds_wsarray* a = atomic_load_explicit(&q->array, memory_order_acquire);
        uint64_t x = atomic_load_explicit(&a->buf[t & (a->size - 1)], memory_order_relaxed);
        if (!atomic_compare_exchange_strong_explicit(&q->top, &t, t + 1, memory_order_seq_cst, memory_order_relaxed))
            return 1;
        *item = x;
        return 0;
    }
    return -1;
}

size_t dyn_wsdeque_size_approx(const dyn_wsdeque_t* q)
{
    int64_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed);
    int64_t t = atomic_load_explicit(&q->top, memory_order_relaxed);
    return b > t ? (size_t)(b - t) : 0;
}
#endif
