/* tests/lib/dyn-xalloc.h -- checked allocation for C tests, benches and tools: xmalloc/xcalloc/xrealloc
 * return the block or print file:line and abort (a zero-size request may return NULL). Each argument is
 * evaluated exactly once, so xrealloc(p, (cap *= 2) * sizeof *p) grows by one doubling. */
#ifndef DYN_XALLOC_H
#define DYN_XALLOC_H

#include <stdio.h>
#include <stdlib.h>

static inline void* dyn_x_check(void* p, size_t n, const char* file, int line)
{
    if (!p && n) {
        fprintf(stderr, "%s:%d: out of memory (%zu bytes)\n", file, line, n);
        abort();
    }
    return p;
}

static inline void* dyn_x_malloc(size_t n, const char* file, int line)
{
    return dyn_x_check(malloc(n), n, file, line);
}

static inline void* dyn_x_calloc(size_t a, size_t b, const char* file, int line)
{
    return dyn_x_check(calloc(a, b), a && b ? (a * b / b == a ? a * b : (size_t)-1) : 0, file, line);
}

static inline void* dyn_x_realloc(void* p, size_t n, const char* file, int line)
{
    return dyn_x_check(realloc(p, n), n, file, line);
}

#define xmalloc(n) dyn_x_malloc((size_t)(n), __FILE__, __LINE__)
#define xcalloc(a, b) dyn_x_calloc((size_t)(a), (size_t)(b), __FILE__, __LINE__)
#define xrealloc(p, n) dyn_x_realloc((p), (size_t)(n), __FILE__, __LINE__)

#endif
