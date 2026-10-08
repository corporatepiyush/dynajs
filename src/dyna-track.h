#ifndef DYNAJS_TRACK_H
#define DYNAJS_TRACK_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

extern _Atomic int dyn_trk_armed;

void* dyn_trk_malloc(size_t size);
void* dyn_trk_malloc_plumbing(size_t size);
void* dyn_trk_calloc_plumbing(size_t nmemb, size_t size);
void* dyn_trk_realloc_plumbing(void* ptr, size_t size);
void* dyn_trk_calloc(size_t nmemb, size_t size);
void* dyn_trk_realloc(void* ptr, size_t size);
void dyn_trk_free(void* ptr);
char* dyn_trk_strdup(const char* s);
char* dyn_trk_strndup(const char* s, size_t n);

#ifdef DYN_TRK_PLUMBING
#define DYN_TRK_MALLOC dyn_trk_malloc_plumbing
#define DYN_TRK_CALLOC dyn_trk_calloc_plumbing
#define DYN_TRK_REALLOC dyn_trk_realloc_plumbing
#else
#define DYN_TRK_MALLOC dyn_trk_malloc
#define DYN_TRK_CALLOC dyn_trk_calloc
#define DYN_TRK_REALLOC dyn_trk_realloc
#endif

static inline int dyn_trk_is_armed(void)
{
    return __builtin_expect(
        atomic_load_explicit(&dyn_trk_armed, memory_order_relaxed), 0);
}

static inline void* dyn_trk_malloc_gate(size_t size)
{
    return dyn_trk_is_armed() ? DYN_TRK_MALLOC(size) : malloc(size);
}

static inline void* dyn_trk_calloc_gate(size_t nmemb, size_t size)
{
    return dyn_trk_is_armed() ? DYN_TRK_CALLOC(nmemb, size)
                              : calloc(nmemb, size);
}

static inline void* dyn_trk_realloc_gate(void* ptr, size_t size)
{
    return dyn_trk_is_armed() ? DYN_TRK_REALLOC(ptr, size)
                              : realloc(ptr, size);
}

static inline void dyn_trk_free_gate(void* ptr)
{
    if (dyn_trk_is_armed())
        dyn_trk_free(ptr);
    else
        free(ptr);
}

static inline char* dyn_trk_strdup_gate(const char* s)
{
    return dyn_trk_is_armed() ? dyn_trk_strdup(s) : strdup(s);
}

static inline char* dyn_trk_strndup_gate(const char* s, size_t n)
{
    return dyn_trk_is_armed() ? dyn_trk_strndup(s, n) : strndup(s, n);
}

#define malloc dyn_trk_malloc_gate
#define calloc dyn_trk_calloc_gate
#define realloc dyn_trk_realloc_gate
#define free dyn_trk_free_gate
#define strdup dyn_trk_strdup_gate
#define strndup dyn_trk_strndup_gate

#endif
