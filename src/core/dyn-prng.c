/*
 * dyn-prng -- xoshiro256** + OS entropy. PURE C. See dyn-prng.h.
 *
 * Moved unchanged from dyna-random.c. The seeding schedule and output function
 * are a compatibility surface: dyna:ml reproduces a fit from a seed, so
 * altering either silently invalidates every recorded model and oracle vector.
 */
#include "dyn-prng.h"

#include <stdlib.h>
#include <string.h>

/* ---------- OS entropy (unseeded PRNG + uuid) ---------- */

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <stdlib.h> /* arc4random_buf */
int dyn_os_entropy(void *buf, size_t n)
{
    arc4random_buf(buf, n);
    return 0;
}
#else
#include <sys/random.h> /* getrandom on Linux */
#include <errno.h>
int dyn_os_entropy(void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    while (n > 0) {
        ssize_t got = getrandom(p, n, 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            /* Fail closed (SP 800-90B 2.2.1/4.3): a failed entropy source
             * must produce no output, and rand() is not an entropy source.
             * The caller throws or aborts; there is no silent fallback. */
            return -1;
        }
        p += got;
        n -= (size_t)got;
    }
    return 0;
}
#endif

/* ---------- xoshiro256** core ---------- */

static inline uint64_t rotl64(uint64_t x, int k)
{
    return (x << k) | (x >> (64 - k));
}

uint64_t dyn_splitmix64(uint64_t *x)
{
    uint64_t z = (*x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

void dyn_prng_seed(dyn_prng_t *r, uint64_t seed)
{
    uint64_t sm = seed;
    r->s[0] = dyn_splitmix64(&sm);
    r->s[1] = dyn_splitmix64(&sm);
    r->s[2] = dyn_splitmix64(&sm);
    r->s[3] = dyn_splitmix64(&sm);
}

uint64_t dyn_prng_next(dyn_prng_t *r)
{
    uint64_t *s = r->s;
    uint64_t result = rotl64(s[1] * 5, 7) * 9;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl64(s[3], 45);
    return result;
}

/* [0, 1) with 53 bits of mantissa precision. */
double dyn_prng_next_double(dyn_prng_t *r)
{
    return (double)(dyn_prng_next(r) >> 11) * (1.0 / 9007199254740992.0);
}

/* Uniform in [0, bound), unbiased via rejection (no modulo bias). */
uint64_t dyn_prng_next_bounded(dyn_prng_t *r, uint64_t bound)
{
    /* threshold = (2^64) mod bound, computed as (-bound) mod bound. */
    uint64_t threshold;
    if (bound == 0)
        return 0;                       /* the modulo below would divide by 0 */
    threshold = (0 - bound) % bound;
    for (;;) {
        uint64_t v = dyn_prng_next(r);
        if (v >= threshold)
            return v % bound;
    }
}

void dyn_prng_fill(dyn_prng_t *restrict r, uint8_t *restrict dst, size_t n)
{
    while (n >= 8) {
        uint64_t v = dyn_prng_next(r);
        memcpy(dst, &v, 8);
        dst += 8;
        n -= 8;
    }
    if (n > 0) {
        uint64_t v = dyn_prng_next(r);
        memcpy(dst, &v, n);
    }
}

/* Seed from the kernel CSPRNG: an unpredictable, unreproducible stream, for
 * callers that want randomness rather than a repeatable sequence. */
int dyn_prng_seed_random(dyn_prng_t *r)
{
    uint64_t seed;
    if (dyn_os_entropy(&seed, sizeof(seed)) < 0)
        return -1;
    dyn_prng_seed(r, seed);
    return 0;
}
