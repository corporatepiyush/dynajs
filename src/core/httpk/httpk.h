#ifndef DYN_HTTPK_HTTPK_H
#define DYN_HTTPK_HTTPK_H

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../dyn-common.h"
#include "../dyn-httpk.h"

#define HTTPK_CHUNK_EXT_MAX 4096u
#define HTTPK_TRAILER_MAX 16384u
#define HTTPK_COOKIE_MAX_BYTES 4096u
#define HTTPK_SINGLETONS 21u

int httpk_singleton_slot(const char* name, size_t n);

static inline int httpk_lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static inline char* httpk_strndup(const char* s, size_t n)
{
    char* p = (char*)malloc(n + 1);
    if (!p)
        return NULL;
    if (n)
        memcpy(p, s, n);
    p[n] = 0;
    return p;
}

#endif
