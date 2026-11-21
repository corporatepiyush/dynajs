#ifndef DYN_FUZZKIT_H
#define DYN_FUZZKIT_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    uint64_t s;
} fk_rng_t;

static int fk_fails;
static uint64_t fk_seed;
static const char* fk_case = "";

#define FK_CHECK(c, ...)                                                          \
    do {                                                                          \
        if (!(c)) {                                                               \
            if (fk_fails++ < 20) {                                                \
                printf("FAIL [%s seed %llu] ", fk_case, (unsigned long long)fk_seed); \
                printf(__VA_ARGS__);                                              \
                printf("\n");                                                     \
            }                                                                     \
        }                                                                         \
    } while (0)

static inline uint64_t fk_next(fk_rng_t* r)
{
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static inline uint32_t fk_below(fk_rng_t* r, uint32_t n)
{
    return n ? (uint32_t)(((fk_next(r) >> 32) * (uint64_t)n) >> 32) : 0;
}

static inline int fk_chance(fk_rng_t* r, uint32_t pct)
{
    return fk_below(r, 100) < pct;
}

static inline uint32_t fk_size(fk_rng_t* r, uint32_t max)
{
    uint32_t k = fk_below(r, 4);
    uint32_t cap = k == 0 ? 4 : k == 1 ? 64 : k == 2 ? 1024 : max;
    return fk_below(r, (cap < max ? cap : max) + 1);
}

static inline void fk_bytes(fk_rng_t* r, uint8_t* p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        p[i] = (uint8_t)fk_next(r);
}

static inline void fk_pick_bytes(fk_rng_t* r, uint8_t* p, size_t n, const char* alphabet)
{
    size_t i, k = strlen(alphabet);
    for (i = 0; i < n; i++)
        p[i] = (uint8_t)alphabet[fk_below(r, (uint32_t)k)];
}

static inline void fk_mutate(fk_rng_t* r, uint8_t* p, size_t n, unsigned edits)
{
    unsigned e;
    if (!n)
        return;
    for (e = 0; e < edits; e++) {
        size_t i = fk_below(r, (uint32_t)n);
        switch (fk_below(r, 4)) {
        case 0:
            p[i] ^= (uint8_t)(1u << fk_below(r, 8));
            break;
        case 1:
            p[i] = (uint8_t)fk_next(r);
            break;
        case 2:
            p[i] = fk_chance(r, 50) ? 0x00 : 0xFF;
            break;
        default:
            p[i] = p[fk_below(r, (uint32_t)n)];
            break;
        }
    }
}

static inline size_t fk_split(fk_rng_t* r, size_t left)
{
    return left <= 1 ? left : 1 + fk_below(r, (uint32_t)(left < 4096 ? left : 4096));
}

static inline unsigned long fk_env(const char* k, unsigned long d)
{
    const char* v = getenv(k);
    return v && *v ? strtoul(v, NULL, 0) : d;
}

static inline void fk_init(const char* name)
{
    const char* v = getenv("DYN_FUZZ_SEED");
    fk_seed = v && *v ? strtoull(v, NULL, 0) : ((uint64_t)time(NULL) << 20) ^ (uint64_t)getpid() ^ (uint64_t)clock();
    printf("%s: seed %llu iters x%lu (replay: DYN_FUZZ_SEED=%llu)\n", name, (unsigned long long)fk_seed, fk_env("DYN_FUZZ_ITERS", 1), (unsigned long long)fk_seed);
}

static inline fk_rng_t fk_stream(const char* case_name)
{
    fk_rng_t r;
    uint64_t h = 1469598103934665603ULL;
    const char* c;
    for (c = case_name; *c; c++)
        h = (h ^ (uint8_t)*c) * 1099511628211ULL;
    r.s = fk_seed ^ h;
    fk_case = case_name;
    return r;
}

static inline unsigned long fk_iters(unsigned long base)
{
    return base * fk_env("DYN_FUZZ_ITERS", 1);
}

static inline int fk_only(const char* case_name)
{
    const char* o = getenv("DYN_FUZZ_ONLY");
    size_t n = strlen(case_name);
    while (o && *o) {
        const char* q = strchr(o, ',');
        size_t l = q ? (size_t)(q - o) : strlen(o);
        if (l == n && !memcmp(o, case_name, n))
            return 1;
        o = q ? q + 1 : NULL;
    }
    o = getenv("DYN_FUZZ_ONLY");
    return !o || !*o;
}

static inline int fk_legacy(void)
{
    const char* o = getenv("DYN_FUZZ_ONLY");
    return !o || !*o;
}

static inline int fk_done(const char* name)
{
    if (fk_fails)
        printf("%s: %d FAILED (replay: DYN_FUZZ_SEED=%llu)\n", name, fk_fails, (unsigned long long)fk_seed);
    else
        printf("%s: all passed (seed %llu)\n", name, (unsigned long long)fk_seed);
    return fk_fails != 0;
}

#endif
