/* tools/alloc-interpose.c -- a malloc-family interposer for the strict
 * base58 allocation gate (tests/run_base58_strict.sh).
 *
 * What it counts: every INTERPOSABLE malloc-family call -- malloc, calloc,
 * realloc (each successful call counts as one allocation event),
 * posix_memalign, aligned_alloc, valloc, strdup, strndup -- whether it comes
 * from the engine's allocator or from plain libc code. Out of band by
 * construction, and documented there rather than silently skipped:
 * malloc_zone_malloc / malloc_zone_free (macOS) and mmap never pass through
 * an interposable symbol, so traffic using them is invisible here -- that is
 * the interposer's boundary, pinned as a scoping row by
 * tests/run_base58_strict.sh (the out-of-band pair staying green is the
 * documented boundary, not a bug). With the boundary stated, this is the
 * "ALL interposable malloc-family traffic, libc included" view the strict
 * claim needs: with the engine's small-block pools bypassed
 * (DYNAJS_MALLOC_POOLS=0) every engine allocation reaches
 * malloc here, so an engine temporary and a libc-direct temporary are
 * equally visible.
 *
 * How it counts: the workload (tests/test_base58_alloc.js) calls
 * std.getenv("##ALLOC-BEGIN <name>##") before its region and
 * std.getenv("##ALLOC-END##") after it. The interposed getenv() watches its
 * argument for those markers and keeps per-window (alloc, free) tallies; at
 * exit it prints one "ALLOCPROBE <name> <allocs> <frees>" line per window to
 * stderr. getenv() is the marker door because macOS stdio flushes through
 * write$NOCANCEL (not the interposable write); a getenv call from the main
 * executable rebinds cleanly. Windows are exact and in-process: no baseline
 * subtraction across processes, no reliance on startup determinism.
 *
 * Platform mechanics:
 *   macOS  -- dyld interposition via __DATA,__interpose. Wrappers are named
 *             ap_* and call the libc entry points DIRECTLY: dyld does not
 *             re-interpose the interposing image's own references, so the
 *             direct call reaches libsystem (verified: dlsym(RTLD_NEXT)
 *             instead returns the interposer itself and recurses).
 *   Linux  -- LD_PRELOAD name replacement; the real entry points come from
 *             dlsym(RTLD_NEXT), with a static bootstrap arena serving the
 *             dlsym-time allocations.
 *
 * Build (done by tests/run_base58_strict.sh):
 *   macOS:  cc -dynamiclib -o alloc_probe.dylib tools/alloc-interpose.c
 *   Linux:  cc -shared -fPIC -o alloc_probe.so tools/alloc-interpose.c -ldl
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#ifndef __APPLE__
#include <dlfcn.h>
#endif

#ifdef __APPLE__
/* dyld interposition needs the wrapper under a different name than the
   libsystem symbol it replaces; LD_PRELOAD on Linux replaces by name. */
#define AP_NAME(x) ap_##x
#else
#define AP_NAME(x) x
#endif

/* --------------------------------------------------- the real entry points */
#ifndef __APPLE__
static void *(*real_malloc)(size_t);
static void *(*real_calloc)(size_t, size_t);
static void *(*real_realloc)(void *, size_t);
static void (*real_free)(void *);
static int (*real_posix_memalign)(void **, size_t, size_t);
static void *(*real_aligned_alloc)(size_t, size_t);
static void *(*real_valloc)(size_t);
static char *(*real_strdup)(const char *);
static char *(*real_strndup)(const char *, size_t);
static char *(*real_getenv)(const char *);
static ssize_t (*real_write)(int, const void *, size_t);
static int real_ready;
static int resolving;             /* 1 while inside resolve_all(): wrapper
                                     traffic is bootstrap-only, never a
                                     nested dlsym */

/* dlsym() may allocate before the real entry points are known; those calls
   are served here and never freed (freed pointers into this arena are
   recognized and ignored). */
static unsigned char boot_buf[1 << 18];
static size_t boot_used;

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
    real_free = dlsym(RTLD_NEXT, "free");
    real_posix_memalign = dlsym(RTLD_NEXT, "posix_memalign");
    real_aligned_alloc = dlsym(RTLD_NEXT, "aligned_alloc");
    real_valloc = dlsym(RTLD_NEXT, "valloc");
    real_strdup = dlsym(RTLD_NEXT, "strdup");
    real_strndup = dlsym(RTLD_NEXT, "strndup");
    real_getenv = dlsym(RTLD_NEXT, "getenv");
    real_write = dlsym(RTLD_NEXT, "write");
    real_ready = real_malloc && real_calloc && real_realloc && real_free
        && real_write;
    resolving = 0;
}
#define AP_ENSURE() do { if (!real_ready && !resolving) resolve_all(); } while (0)
#define AP_READY()  (real_ready)
#else /* __APPLE__: direct calls reach libsystem; always usable */
#define AP_ENSURE() do { } while (0)
#define AP_READY()  1
static int boot_ptr(const void *p) { (void)p; return 0; }
#endif

/* ------------------------------------------------------------- window table */
#define MAX_WINDOWS 32
#define WIN_NAME    48
static struct {
    char name[WIN_NAME];
    unsigned long long allocs, frees;
} wins[MAX_WINDOWS];
static int n_wins;
static int cur_win = -1;          /* -1 = outside every window */

static int win_find(const char *name, size_t n)
{
    int i;
    for (i = 0; i < n_wins; i++)
        if (strlen(wins[i].name) == n && memcmp(wins[i].name, name, n) == 0)
            return i;
    if (n_wins == MAX_WINDOWS || n >= WIN_NAME)
        return -1;
    memcpy(wins[n_wins].name, name, n);
    wins[n_wins].name[n] = 0;
    wins[n_wins].allocs = wins[n_wins].frees = 0;
    return n_wins++;
}

static void count_alloc(void)
{
    if (cur_win >= 0)
        wins[cur_win].allocs++;
}
static void count_free(void)
{
    if (cur_win >= 0)
        wins[cur_win].frees++;
}

/* ------------------------------------------------------- marker scan on write */
static void scan_markers(const char *buf, size_t n)
{
    size_t i = 0;
    while (i + 8 <= n) {
        const char *p = buf + i;
        if (p[0] == '#' && memcmp(p, "##ALLOC-", 8) == 0) {
            if (i + 13 <= n && memcmp(p, "##ALLOC-END##", 13) == 0) {
                cur_win = -1;
                i += 13;
                continue;
            }
            if (i + 14 <= n && memcmp(p, "##ALLOC-BEGIN ", 14) == 0) {
                /* the window name runs to the closing "##" */
                size_t j = i + 14;
                while (j + 2 <= n && memcmp(buf + j, "##", 2) != 0)
                    j++;
                if (j + 2 <= n) {
                    cur_win = win_find(buf + i + 14, j - (i + 14));
                    i = j + 2;
                    continue;
                }
            }
        }
        i++;
    }
}

static void dump_windows(void)
{
    char line[256];
    int i;
    AP_ENSURE();
    for (i = 0; i < n_wins; i++) {
        int len = snprintf(line, sizeof line, "ALLOCPROBE %s %llu %llu\n",
                           wins[i].name, wins[i].allocs, wins[i].frees);
        if (len > 0 && AP_READY()) {
#ifdef __APPLE__
            if (write(2, line, (size_t)len) < 0) {
                /* diagnostics only: a failed dump must not perturb the probe */
            }
#else
            if (real_write(2, line, (size_t)len) < 0) {
                /* diagnostics only: a failed dump must not perturb the probe */
            }
#endif
        }
    }
}

__attribute__((constructor)) static void probe_init(void)
{
    AP_ENSURE();
    atexit(dump_windows);
}

/* ---------------------------------------------------------- wrapped mallocs */
void *AP_NAME(malloc)(size_t n)
{
    void *p;
#ifndef __APPLE__
    if (!AP_READY()) {
        if (!resolving)
            resolve_all();
        if (!AP_READY())
            return boot_alloc(n ? n : 1);   /* NULL if the arena is spent */
    }
    p = real_malloc(n ? n : 1);
#else
    p = malloc(n ? n : 1);
#endif
    if (p)
        count_alloc();
    return p;
}

void *AP_NAME(calloc)(size_t a, size_t b)
{
    void *p;
    size_t tot = a * b;
#ifndef __APPLE__
    if (!AP_READY()) {
        if (!resolving)
            resolve_all();
        if (!AP_READY()) {
            p = boot_alloc(tot ? tot : 1);
            if (p)
                memset(p, 0, tot);
            return p;
        }
    }
    p = real_calloc(a, b);
#else
    p = calloc(a, b);
#endif
    if (p)
        count_alloc();
    return p;
}

void *AP_NAME(realloc)(void *old, size_t n)
{
    void *p;
    if (old && boot_ptr(old))
        return old;                 /* Linux bootstrap block: size is tiny */
#ifndef __APPLE__
    if (!AP_READY() && !resolving)
        resolve_all();
    if (!AP_READY())
        return boot_alloc(n ? n : 1);
    p = real_realloc(old, n ? n : 1);
#else
    p = realloc(old, n ? n : 1);
#endif
    if (p)
        count_alloc();              /* every successful realloc is one event */
    return p;
}

void AP_NAME(free)(void *p)
{
    if (!p || boot_ptr(p))
        return;
#ifndef __APPLE__
    if (!AP_READY())
        return;                     /* nothing real exists before resolution */
    real_free(p);
#else
    free(p);
#endif
    count_free();
}

int AP_NAME(posix_memalign)(void **out, size_t align, size_t n)
{
    int rc;
    AP_ENSURE();
#ifndef __APPLE__
    if (!AP_READY() || !real_posix_memalign)
        return ENOMEM;
    rc = real_posix_memalign(out, align, n);
#else
    rc = posix_memalign(out, align, n);
#endif
    if (rc == 0 && out && *out)
        count_alloc();
    return rc;
}

void *AP_NAME(aligned_alloc)(size_t align, size_t n)
{
    void *p = NULL;
    AP_ENSURE();
#ifndef __APPLE__
    if (AP_READY() && real_aligned_alloc)
        p = real_aligned_alloc(align, n);
#else
    p = aligned_alloc(align, n);
#endif
    if (p)
        count_alloc();
    return p;
}

void *AP_NAME(valloc)(size_t n)
{
    void *p = NULL;
    AP_ENSURE();
#ifndef __APPLE__
    if (AP_READY() && real_valloc)
        p = real_valloc(n);
#else
    p = valloc(n);
#endif
    if (p)
        count_alloc();
    return p;
}

char *AP_NAME(strdup)(const char *s)
{
    char *p = NULL;
    AP_ENSURE();
#ifndef __APPLE__
    if (AP_READY() && real_strdup)
        p = real_strdup(s);
#else
    p = strdup(s);
#endif
    if (p)
        count_alloc();
    return p;
}

char *AP_NAME(strndup)(const char *s, size_t n)
{
    char *p = NULL;
    AP_ENSURE();
#ifndef __APPLE__
    if (AP_READY() && real_strndup)
        p = real_strndup(s, n);
#else
    p = strndup(s, n);
#endif
    if (p)
        count_alloc();
    return p;
}

/* marker door: std.getenv("##ALLOC-BEGIN x##") / std.getenv("##ALLOC-END##")
   open and close the counting windows exactly around the region body */
char *AP_NAME(getenv)(const char *name)
{
    if (name && name[0] == '#' && name[1] == '#')
        scan_markers(name, strlen(name));
#ifndef __APPLE__
    AP_ENSURE();
    if (!AP_READY() || !real_getenv)
        return NULL;
    return real_getenv(name);
#else
    return getenv(name);   /* direct: dyld does not re-interpose our own refs */
#endif
}

#ifdef __APPLE__
/* dyld interposition: the __DATA,__interpose section rebinds every image's
   references to these libsystem symbols to the ap_* wrappers above. The
   replacee address is the libsystem definition (declared here, never
   defined). */
typedef struct { const void *replacement; const void *replacee; } ap_interpose_t;
#define AP_INTERPOSE(name, orig) \
    __attribute__((used)) static const ap_interpose_t ap_i_##orig \
        __attribute__((section("__DATA,__interpose"))) = \
            { (const void *)(void *)AP_NAME(name), (const void *)(void *)orig };
AP_INTERPOSE(malloc, malloc)
AP_INTERPOSE(calloc, calloc)
AP_INTERPOSE(realloc, realloc)
AP_INTERPOSE(free, free)
AP_INTERPOSE(posix_memalign, posix_memalign)
AP_INTERPOSE(aligned_alloc, aligned_alloc)
AP_INTERPOSE(valloc, valloc)
AP_INTERPOSE(strdup, strdup)
AP_INTERPOSE(strndup, strndup)
AP_INTERPOSE(getenv, getenv)
#endif
