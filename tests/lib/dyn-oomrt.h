#ifndef DYN_OOMRT_H
#define DYN_OOMRT_H
#include <stddef.h>
#include <stdlib.h>
#include "dynajs.h"
#if defined(__SANITIZE_ADDRESS__)
#define OOMRT_BT 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define OOMRT_BT 1
#endif
#endif
#ifdef OOMRT_BT
#include <sanitizer/common_interface_defs.h>
#endif

static size_t oomrt_cap = (size_t)-1;
static size_t oomrt_refused;
static long oomrt_fail_at;

typedef union {
    max_align_t align;
    size_t n;
} oomrt_hdr;

static int oomrt_refuse(const JSMallocState* s, size_t grow)
{
    if (oomrt_fail_at > 0 && --oomrt_fail_at == 0) {
        oomrt_refused++;
#ifdef OOMRT_BT
        if (getenv("OOM_BT"))
            __sanitizer_print_stack_trace();
#endif
        return 1;
    }
    if (oomrt_cap == (size_t)-1 || (grow <= oomrt_cap && s->malloc_size <= oomrt_cap - grow))
        return 0;
    oomrt_refused++;
    return 1;
}

static void* oomrt_malloc(JSMallocState* s, size_t n)
{
    oomrt_hdr* h;
    if (n == 0)
        n = 1;
    if (oomrt_refuse(s, n))
        return NULL;
    h = malloc(sizeof(*h) + n);
    if (!h)
        return NULL;
    h->n = n;
    s->malloc_count++;
    s->malloc_size += n;
    return h + 1;
}

static void oomrt_free(JSMallocState* s, void* p)
{
    oomrt_hdr* h;
    if (!p)
        return;
    h = (oomrt_hdr*)p - 1;
    s->malloc_count--;
    s->malloc_size -= h->n;
    free(h);
}

static void* oomrt_realloc(JSMallocState* s, void* p, size_t n)
{
    oomrt_hdr *h, *nh;
    size_t old;
    if (!p)
        return n ? oomrt_malloc(s, n) : NULL;
    if (n == 0) {
        oomrt_free(s, p);
        return NULL;
    }
    h = (oomrt_hdr*)p - 1;
    old = h->n;
    if (n > old && oomrt_refuse(s, n - old))
        return NULL;
    nh = realloc(h, sizeof(*nh) + n);
    if (!nh)
        return NULL;
    nh->n = n;
    s->malloc_size = s->malloc_size - old + n;
    return nh + 1;
}

static size_t oomrt_usable(const void* p)
{
    return p ? ((const oomrt_hdr*)p - 1)->n : 0;
}

static const JSMallocFunctions oomrt_mf = { oomrt_malloc, oomrt_free, oomrt_realloc, oomrt_usable };

static JSRuntime* oomrt_new(void)
{
    return JS_NewRuntime2(&oomrt_mf, NULL);
}
#endif
