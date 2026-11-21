#if defined(__GLIBC__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
#include "cryptok.h"

#if defined(__GLIBC__) || defined(__OpenBSD__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__DragonFly__) || defined(__sun__)
void dyn_wipe(void* p, size_t n)
{
    explicit_bzero(p, n);
}
#else
static void dyn_wipe_impl(void* p, size_t n)
{
    memset(p, 0, n);
}
static void (*volatile dyn_wipe_fn)(void*, size_t) = dyn_wipe_impl;
void dyn_wipe(void* p, size_t n)
{
    dyn_wipe_fn(p, n);
}
#endif
