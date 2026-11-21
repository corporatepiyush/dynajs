#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif
#include "svg-int.h"

#if defined(SVG2IMG_SLAB) && !defined(SVG2IMG_ALLOC_HOOKS)

#include <stdint.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifndef SLAB_MAX
#define SLAB_MAX        2048u
#endif
#define SLAB_ALIGN      16u
#define SLAB_HDR        64u
#ifndef SLAB_RESERVE
#define SLAB_RESERVE    (64u * 1024u * 1024u)
#endif

#ifndef SLAB_CHUNK
#define SLAB_CHUNK      (256u * 1024u)
#endif

static const uint16_t slab_classes[] = {
    16,   32,   48,   64,   80,   96,  112,  128,
   144,  160,  176,  192,  208,  224,  240,  256,
   288,  320,  352,  384,  416,  448,  480,  512,
   576,  640,  704,  768,  832,  896,  960, 1024,
  1152, 1280, 1408, 1536, 1664, 1792, 1920, 2048,
  2304, 2560, 2816, 3072, 3328, 3584, 3840, 4096
};
#define SLAB_NCLASS ((int)(sizeof(slab_classes) / sizeof(slab_classes[0])))

typedef struct {
    uint16_t cls;
    uint16_t pad;
    uint32_t pad2;
} SlabPage;

typedef struct {
    unsigned char *base;
    unsigned char *mapped;
    unsigned char *cur;
    size_t         size;
    size_t         page_size;
    int            failed;
    void*          free_list[SLAB_NCLASS];
    unsigned char* page[SLAB_NCLASS];
    uint32_t       bump[SLAB_NCLASS];
    uint32_t       end[SLAB_NCLASS];
} Slab;

_Static_assert(SLAB_HDR % SLAB_ALIGN == 0, "slab header keeps 16-byte alignment");

static _Thread_local Slab g_slab;

static int slab_init(void) {
#if defined(__unix__) || defined(__APPLE__)
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0 || ((size_t)ps & ((size_t)ps - 1)) != 0 ||
        (size_t)ps < SLAB_HDR || (size_t)ps > 65536) {
        g_slab.failed = 1;
        return 0;
    }
    void* p = mmap(NULL, SLAB_RESERVE, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        g_slab.failed = 1;
        return 0;
    }
    g_slab.base = (unsigned char*)p;
    g_slab.mapped = g_slab.base;
    g_slab.cur = g_slab.base;
    g_slab.size = SLAB_RESERVE;
    g_slab.page_size = (size_t)ps;
    return 1;
#else
    g_slab.failed = 1;
    return 0;
#endif
}

static int slab_class(size_t n) {
    for (int i = 0; i < SLAB_NCLASS; i++)
        if (n <= (size_t)slab_classes[i]) return i;
    return -1;
}

static int slab_in_arena(const void* p) {
    const unsigned char* q = (const unsigned char*)p;
    return g_slab.base != NULL && q >= g_slab.base && q < g_slab.cur;
}

static unsigned char* slab_new_page(int cls) {
#if defined(__unix__) || defined(__APPLE__)
    size_t ps = g_slab.page_size;
    if (g_slab.cur + ps > g_slab.base + g_slab.size) return NULL;
    if (g_slab.cur + ps > g_slab.mapped) {
        size_t left = (size_t)(g_slab.base + g_slab.size - g_slab.mapped);
        size_t chunk = left < SLAB_CHUNK ? left : SLAB_CHUNK;
        chunk &= ~(ps - 1);
        if (chunk == 0 ||
            mprotect(g_slab.mapped, chunk, PROT_READ | PROT_WRITE) != 0) {
            g_slab.failed = 1;
            return NULL;
        }
        g_slab.mapped += chunk;
    }
    unsigned char* p = g_slab.cur;
    g_slab.cur += ps;
    ((SlabPage*)p)->cls = (uint16_t)cls;
    g_slab.page[cls] = p;
    g_slab.bump[cls] = SLAB_HDR;
    g_slab.end[cls] = (uint32_t)ps;
    return p;
#else
    (void)cls;
    return NULL;
#endif
}

static void* slab_alloc(size_t n) {
    if (n == 0) n = 1;
    if (n > SLAB_MAX) return malloc(n);
    if (g_slab.base == NULL) {
        if (g_slab.failed || !slab_init()) return malloc(n);
    }

    int cls = slab_class(n);
    void* f = g_slab.free_list[cls];
    if (f) {
        g_slab.free_list[cls] = *(void**)f;
        return f;
    }
    unsigned char* pg = g_slab.page[cls];
    if (pg == NULL || g_slab.bump[cls] + slab_classes[cls] > g_slab.end[cls]) {
        pg = slab_new_page(cls);
        if (pg == NULL) return malloc(n);
    }
    void* p = pg + g_slab.bump[cls];
    g_slab.bump[cls] += slab_classes[cls];
    return p;
}

static void slab_free(void* p) {
    if (p == NULL) return;
    if (!slab_in_arena(p)) {
        free(p);
        return;
    }
    size_t mask = g_slab.page_size - 1;
    SlabPage* pg = (SlabPage*)((uintptr_t)p & ~(uintptr_t)mask);
    int cls = (int)pg->cls;
    *(void**)p = g_slab.free_list[cls];
    g_slab.free_list[cls] = p;
}

#if defined(SVG2IMG_DOC_ARENA)

#define DOC_ARENA_RESERVE   (32u * 1024u * 1024u)
#define DOC_ARENA_CHUNK     (1024u * 1024u)
#define DOC_ARENA_MAX_ALLOC 65536u
#define DOC_ARENA_HDR       16u

typedef struct {
    unsigned char *base;
    unsigned char *mapped;
    unsigned char *cur;
    size_t         size;
    size_t         page_size;
    int            active;
    int            failed;
} DocArena;

static _Thread_local DocArena g_doc;

static int doc_arena_init(void) {
#if defined(__unix__) || defined(__APPLE__)
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0 || ((size_t)ps & ((size_t)ps - 1)) != 0) {
        g_doc.failed = 1;
        return 0;
    }
    void* p = mmap(NULL, DOC_ARENA_RESERVE, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        g_doc.failed = 1;
        return 0;
    }
    g_doc.base = (unsigned char*)p;
    g_doc.mapped = g_doc.base;
    g_doc.cur = g_doc.base;
    g_doc.size = DOC_ARENA_RESERVE;
    g_doc.page_size = (size_t)ps;
    return 1;
#else
    g_doc.failed = 1;
    return 0;
#endif
}

static int doc_arena_commit(size_t need) {
#if defined(__unix__) || defined(__APPLE__)
    if (g_doc.cur + need <= g_doc.mapped) return 1;
    size_t left = (size_t)(g_doc.base + g_doc.size - g_doc.mapped);
    size_t ps = g_doc.page_size;
    size_t chunk = DOC_ARENA_CHUNK;
    if (chunk < need) chunk = (need + ps - 1) & ~(ps - 1);
    if (chunk > left) chunk = left & ~(ps - 1);
    if (chunk == 0 ||
        mprotect(g_doc.mapped, chunk, PROT_READ | PROT_WRITE) != 0) {
        g_doc.failed = 1;
        return 0;
    }
    g_doc.mapped += chunk;
    return 1;
#else
    (void)need;
    g_doc.failed = 1;
    return 0;
#endif
}

static void* doc_arena_alloc(size_t n) {
    if (n == 0) n = 1;
    if (n > DOC_ARENA_MAX_ALLOC) return slab_alloc(n);
    if (g_doc.base == NULL && !doc_arena_init()) return slab_alloc(n);
    if (g_doc.failed) return slab_alloc(n);
    size_t total = (n + DOC_ARENA_HDR + 15u) & ~(size_t)15u;
    if (!doc_arena_commit(total)) return slab_alloc(n);
    unsigned char* base = g_doc.cur;
    g_doc.cur += total;
    *(size_t*)base = total - DOC_ARENA_HDR;
    return base + DOC_ARENA_HDR;
}

static int doc_arena_owns(const void* p) {
    const unsigned char* q = (const unsigned char*)p;
    return g_doc.base != NULL && q >= g_doc.base + DOC_ARENA_HDR &&
           q < g_doc.cur;
}

static void* doc_arena_realloc(void* p, size_t n) {
    if (n == 0) n = 1;
    size_t cap = *(size_t*)((unsigned char*)p - DOC_ARENA_HDR);
    if (n <= cap) return p;
    void* q = doc_arena_alloc(n);
    if (q == NULL) return NULL;
    memcpy(q, p, cap);
    return q;
}

void svg_doc_arena_begin(void) {
#if defined(__unix__) || defined(__APPLE__)
    if (g_doc.base == NULL && !doc_arena_init()) return;
    g_doc.active = !g_doc.failed;
#else
    (void)0;
#endif
}

void svg_doc_arena_pause(void) {
    g_doc.active = 0;
}

void svg_doc_arena_reset(void) {
    g_doc.active = 0;
#if defined(__unix__) || defined(__APPLE__)
    if (g_doc.base != NULL && g_doc.mapped > g_doc.base) {
#ifdef MADV_FREE
        (void)madvise(g_doc.base, (size_t)(g_doc.mapped - g_doc.base),
                      MADV_FREE);
#endif
    }
    g_doc.cur = g_doc.base;
#endif
}

#endif

static _Thread_local int g_libc_only;

void svg_alloc_worker(int on) { g_libc_only = on; }

void* svg_malloc(size_t n) {
    if (g_libc_only) return malloc(n ? n : 1);
#if defined(SVG2IMG_DOC_ARENA)
    if (g_doc.active) return doc_arena_alloc(n);
#endif
    return slab_alloc(n);
}

void* svg_calloc(size_t n) {
    if (g_libc_only) return calloc(1, n ? n : 1);
#if defined(SVG2IMG_DOC_ARENA)
    if (g_doc.active) {
        void* p = doc_arena_alloc(n);
        if (p) memset(p, 0, n);
        return p;
    }
#endif
    void* p = slab_alloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void* svg_realloc(void* p, size_t n) {
#if defined(SVG2IMG_DOC_ARENA)
    if (doc_arena_owns(p)) return doc_arena_realloc(p, n);
#endif
    if (p == NULL) return g_libc_only ? malloc(n ? n : 1) : slab_alloc(n);
    if (n == 0) n = 1;
    if (!slab_in_arena(p)) return realloc(p, n);

    size_t mask = g_slab.page_size - 1;
    SlabPage* pg = (SlabPage*)((uintptr_t)p & ~(uintptr_t)mask);
    size_t cap = slab_classes[pg->cls];
    if (n <= cap) return p;

    void* q = slab_alloc(n);
    if (q == NULL) return NULL;
    memcpy(q, p, cap);
    slab_free(p);
    return q;
}

void svg_mfree_slow(void* p) {
#if defined(SVG2IMG_DOC_ARENA)
    if (doc_arena_owns(p)) return;
#endif
    slab_free(p);
}

#elif defined(SVG2IMG_ALLOC_HOOKS)

static svg_alloc_fn   g_alloc   = malloc;
static svg_realloc_fn g_realloc = realloc;
static svg_free_fn    g_free    = free;

void svg_alloc_worker(int on) { (void)on; }

void svg_set_alloc_hooks(svg_alloc_fn alloc, svg_realloc_fn realloc_fn,
                         svg_free_fn free_fn) {
    g_alloc   = alloc      ? alloc      : malloc;
    g_realloc = realloc_fn ? realloc_fn : realloc;
    g_free    = free_fn    ? free_fn    : free;
}

void* svg_malloc(size_t n) {
    return g_alloc(n ? n : 1);
}

void* svg_realloc(void* p, size_t n) {
    return g_realloc(p, n ? n : 1);
}

void* svg_calloc(size_t n) {
    void* p = svg_malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void svg_mfree(void* p) {
    if (p) g_free(p);
}

#endif

#if !defined(SVG2IMG_SLAB) || !defined(SVG2IMG_DOC_ARENA) || \
    defined(SVG2IMG_ALLOC_HOOKS)
void svg_doc_arena_begin(void) {}
void svg_doc_arena_pause(void) {}
void svg_doc_arena_reset(void) {}
#endif

#define SCRATCH_CAP (64u * 1024u)
#define SCRATCH_FB  16

typedef struct {
    void*  p;
    size_t at;
} ScratchFallback;

typedef struct {
    size_t used;
    int nfb;
    ScratchFallback fb[SCRATCH_FB];
    _Alignas(16) unsigned char buf[SCRATCH_CAP];
} Scratch;

static _Thread_local Scratch g_scratch;

size_t svg_scratch_mark(void) {
    return g_scratch.used;
}

void* svg_scratch_alloc(size_t n) {
    if (n == 0) n = 1;
    size_t off = (g_scratch.used + 15u) & ~(size_t)15u;
    if (off <= SCRATCH_CAP && n <= SCRATCH_CAP - off) {
        g_scratch.used = off + n;
        return g_scratch.buf + off;
    }
    if (g_scratch.nfb < SCRATCH_FB) {
        void* p = svg_malloc(n);
        if (p) {
            g_scratch.fb[g_scratch.nfb].p = p;
            g_scratch.fb[g_scratch.nfb].at = g_scratch.used;
            g_scratch.nfb++;
        }
        return p;
    }
    return NULL;
}

void* svg_scratch_try(size_t n) {
    if (n == 0) n = 1;
    size_t off = (g_scratch.used + 15u) & ~(size_t)15u;
    if (off <= SCRATCH_CAP && n <= SCRATCH_CAP - off) {
        g_scratch.used = off + n;
        return g_scratch.buf + off;
    }
    return NULL;
}

void svg_scratch_release(size_t mark) {
    while (g_scratch.nfb > 0 &&
           g_scratch.fb[g_scratch.nfb - 1].at >= mark) {
        g_scratch.nfb--;
        svg_mfree(g_scratch.fb[g_scratch.nfb].p);
    }
    g_scratch.used = mark;
}

#if !defined(SVG2IMG_SLAB) && !defined(SVG2IMG_ALLOC_HOOKS)

void svg_alloc_worker(int on) { (void)on; }
#endif

typedef int svg_alloc_unit_placeholder;
