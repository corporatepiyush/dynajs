#include "decnum.h"

static void* dyn_dec_std_calloc(size_t n, size_t sz)
{
    return calloc(n, sz);
}

static void* dyn_dec_std_malloc(size_t n)
{
    return malloc(n);
}

static void* dyn_dec_std_realloc(void* p, size_t n)
{
    return realloc(p, n);
}

static void dyn_dec_std_free(void* p)
{
    free(p);
}

static const dyn_dec_allocator_t dyn_dec_std_allocator = {
    dyn_dec_std_malloc,
    dyn_dec_std_calloc,
    dyn_dec_std_realloc,
    dyn_dec_std_free,
};

_Atomic(const dyn_dec_allocator_t*) dyn_dec_allocator = &dyn_dec_std_allocator;

void dyn_dec_set_allocator(const dyn_dec_allocator_t* a)
{
    atomic_store_explicit(&dyn_dec_allocator, a ? a : &dyn_dec_std_allocator,
        memory_order_relaxed);
}
