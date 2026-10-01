#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <execinfo.h>
#ifndef __APPLE__
#include <dlfcn.h>
#endif

#ifdef __APPLE__
#define AF_NAME(x) af_##x
#else
#define AF_NAME(x) x
#endif

#ifndef __APPLE__
static void *(*real_malloc)(size_t);
static void *(*real_calloc)(size_t, size_t);
static void *(*real_realloc)(void *, size_t);
static int (*real_posix_memalign)(void **, size_t, size_t);
static void *(*real_aligned_alloc)(size_t, size_t);
static void *(*real_valloc)(size_t);
static char *(*real_strdup)(const char *);
static char *(*real_strndup)(const char *, size_t);
static char *(*real_getenv)(const char *);
static int real_ready;
static int resolving;

static unsigned char boot_buf[1 << 18];
static size_t boot_used;

static int boot_ptr(const void *p) __attribute__((unused));
static int boot_ptr(const void *p)
{
    return p >= (const void *)boot_buf
        && p < (const void *)(boot_buf + sizeof boot_buf);
}

static void *boot_alloc(size_t n)
{
    void *p;
    n = (n + 15) & ~(size_t)15;
    if (boot_used + n > sizeof boot_buf)
        return NULL;
    p = boot_buf + boot_used;
    boot_used += n;
    return p;
}

static void resolve_all(void)
{
    if (real_ready || resolving)
        return;
    resolving = 1;
    real_malloc = dlsym(RTLD_NEXT, "malloc");
    real_calloc = dlsym(RTLD_NEXT, "calloc");
    real_realloc = dlsym(RTLD_NEXT, "realloc");
    real_posix_memalign = dlsym(RTLD_NEXT, "posix_memalign");
    real_aligned_alloc = dlsym(RTLD_NEXT, "aligned_alloc");
    real_valloc = dlsym(RTLD_NEXT, "valloc");
    real_strdup = dlsym(RTLD_NEXT, "strdup");
    real_strndup = dlsym(RTLD_NEXT, "strndup");
    real_getenv = dlsym(RTLD_NEXT, "getenv");
    real_ready = real_malloc && real_calloc && real_realloc;
    resolving = 0;
}
#define AF_ENSURE() do { if (!real_ready && !resolving) resolve_all(); } while (0)
#else
#define AF_ENSURE() do { } while (0)
static int boot_ptr(const void *p) { (void)p; return 0; }
#endif

static int af_armed;
static long af_at;
static long af_counted;
static int af_fired;

static int af_tick(void)
{
    if (!af_armed || af_fired)
        return 0;
    af_counted++;
    if (af_counted == af_at) {
        af_fired = 1;
        fprintf(stderr, "ALLOCF: fail %ld\n", af_counted);
        if (getenv("DYNA_FAIL_ALLOC_BACKTRACE")) {
            void *bt[24];
            int n = backtrace(bt, 24);
            fprintf(stderr, "ALLOCF: backtrace:\n");
            backtrace_symbols_fd(bt, n, 2);
        }
        return 1;
    }
    return 0;
}

static void af_arm_from_env(void)
{
    const char *s;
    AF_ENSURE();
#ifdef __APPLE__
    s = getenv("DYNA_FAIL_ALLOC_AT");
#else
    s = real_getenv ? real_getenv("DYNA_FAIL_ALLOC_AT") : NULL;
#endif
    af_at = s ? strtol(s, NULL, 10) : 0;
    af_armed = 1;
    fprintf(stderr, "ALLOCF: armed at=%ld\n", af_at);
}

static void af_dump(void) __attribute__((destructor));
static void af_dump(void)
{
    if (af_armed)
        fprintf(stderr, "ALLOCF: exit counted=%ld fired=%d\n",
                af_counted, af_fired);
}

void *AF_NAME(malloc)(size_t n)
{
#ifdef __APPLE__
    if (af_tick())
        return NULL;
    return malloc(n);
#else
    void *p;
    AF_ENSURE();
    if (!real_ready)
        return boot_alloc(n);
    if (af_tick())
        return NULL;
    p = real_malloc(n);
    return p;
#endif
}

void *AF_NAME(calloc)(size_t a, size_t b)
{
#ifdef __APPLE__
    if (af_tick())
        return NULL;
    return calloc(a, b);
#else
    AF_ENSURE();
    if (!real_ready) {
        size_t n = a * b;
        void *p = boot_alloc(n ? n : 1);
        if (p)
            memset(p, 0, n);
        return p;
    }
    if (af_tick())
        return NULL;
    return real_calloc(a, b);
#endif
}

void *AF_NAME(realloc)(void *p, size_t n)
{
#ifdef __APPLE__
    if (af_tick())
        return NULL;
    return realloc(p, n);
#else
    AF_ENSURE();
    if (!real_ready) {
        void *q = boot_alloc(n);
        return q;
    }
    if (af_tick())
        return NULL;
    return real_realloc(p, n);
#endif
}

int AF_NAME(posix_memalign)(void **out, size_t align, size_t n)
{
#ifdef __APPLE__
    if (af_tick())
        return ENOMEM;
    return posix_memalign(out, align, n);
#else
    AF_ENSURE();
    if (!real_ready) {
        void *p = boot_alloc(n + align);
        if (!p)
            return ENOMEM;
        *out = (void *)(((uintptr_t)p + align - 1) & ~(uintptr_t)(align - 1));
        return 0;
    }
    if (af_tick())
        return ENOMEM;
    return real_posix_memalign(out, align, n);
#endif
}

void *AF_NAME(aligned_alloc)(size_t align, size_t n)
{
#ifdef __APPLE__
    if (af_tick())
        return NULL;
    return aligned_alloc(align, n);
#else
    AF_ENSURE();
    if (!real_ready)
        return boot_alloc(n + align);
    if (af_tick())
        return NULL;
    return real_aligned_alloc ? real_aligned_alloc(align, n) : NULL;
#endif
}

void *AF_NAME(valloc)(size_t n)
{
#ifdef __APPLE__
    if (af_tick())
        return NULL;
    return valloc(n);
#else
    AF_ENSURE();
    if (!real_ready)
        return boot_alloc(n);
    if (af_tick())
        return NULL;
    return real_valloc ? real_valloc(n) : NULL;
#endif
}

char *AF_NAME(strdup)(const char *s)
{
#ifdef __APPLE__
    if (af_tick())
        return NULL;
    return strdup(s);
#else
    size_t n;
    char *p;
    AF_ENSURE();
    if (!real_ready) {
        n = strlen(s) + 1;
        p = boot_alloc(n);
        if (p)
            memcpy(p, s, n);
        return p;
    }
    if (af_tick())
        return NULL;
    return real_strdup ? real_strdup(s) : NULL;
#endif
}

char *AF_NAME(strndup)(const char *s, size_t n)
{
#ifdef __APPLE__
    if (af_tick())
        return NULL;
    return strndup(s, n);
#else
    AF_ENSURE();
    if (!real_ready)
        return NULL;
    if (af_tick())
        return NULL;
    return real_strndup ? real_strndup(s, n) : NULL;
#endif
}

char *AF_NAME(getenv)(const char *name)
{
#ifdef __APPLE__
    if (!af_armed && name && strcmp(name, "##ALLOC-ARM##") == 0) {
        af_arm_from_env();
        return NULL;
    }
    return getenv(name);
#else
    AF_ENSURE();
    if (!af_armed && name && strcmp(name, "##ALLOC-ARM##") == 0) {
        af_arm_from_env();
        return NULL;
    }
    return real_getenv ? real_getenv(name) : NULL;
#endif
}

#ifdef __APPLE__
__attribute__((used)) static void *af_pairs[]
__attribute__((section("__DATA,__interpose"))) = {
    (void *)AF_NAME(malloc), (void *)malloc,
    (void *)AF_NAME(calloc), (void *)calloc,
    (void *)AF_NAME(realloc), (void *)realloc,
    (void *)AF_NAME(posix_memalign), (void *)posix_memalign,
    (void *)AF_NAME(aligned_alloc), (void *)aligned_alloc,
    (void *)AF_NAME(valloc), (void *)valloc,
    (void *)AF_NAME(strdup), (void *)strdup,
    (void *)AF_NAME(strndup), (void *)strndup,
    (void *)AF_NAME(getenv), (void *)getenv,
};
#endif
