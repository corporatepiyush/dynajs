#include "ds-internal.h"
#include "ds-alloc.h"

#ifdef DYN_DS_HOOKS
static _Atomic(void* (*)(size_t)) ds_malloc_fn = malloc;
static _Atomic(void* (*)(void*, size_t)) ds_realloc_fn = realloc;
static _Atomic(void (*)(void*)) ds_free_fn = free;

void* ds_hook_malloc(size_t n)
{
    return atomic_load_explicit(&ds_malloc_fn, memory_order_relaxed)(n);
}

void* ds_hook_realloc(void* p, size_t n)
{
    return atomic_load_explicit(&ds_realloc_fn, memory_order_relaxed)(p, n);
}

void ds_hook_free(void* p)
{
    atomic_load_explicit(&ds_free_fn, memory_order_relaxed)(p);
}

void* ds_hook_calloc(size_t n, size_t s)
{
    void* p;
    if (s && n > SIZE_MAX / s)
        return NULL;
    p = ds_hook_malloc(n * s);
    if (p)
        memset(p, 0, n * s);
    return p;
}

void dyn_ds_set_allocator(void* (*malloc_fn)(size_t), void* (*realloc_fn)(void*, size_t), void (*free_fn)(void*))
{
    atomic_store(&ds_malloc_fn, malloc_fn ? malloc_fn : malloc);
    atomic_store(&ds_realloc_fn, realloc_fn ? realloc_fn : realloc);
    atomic_store(&ds_free_fn, free_fn ? free_fn : free);
}
#else
void dyn_ds_set_allocator(void* (*malloc_fn)(size_t), void* (*realloc_fn)(void*, size_t), void (*free_fn)(void*))
{
    (void)malloc_fn;
    (void)realloc_fn;
    (void)free_fn;
}
#endif

void* dyn_ds_malloc(size_t n)
{
    return DYN_DS_MALLOC(n);
}

void* dyn_ds_calloc(size_t n, size_t size)
{
    return DYN_DS_CALLOC(n, size);
}

void* dyn_ds_realloc(void* p, size_t n)
{
    return DYN_DS_REALLOC(p, n);
}

void dyn_ds_free(void* p)
{
    DYN_DS_FREE(p);
}
