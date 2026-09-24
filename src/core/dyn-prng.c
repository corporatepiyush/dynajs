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
    uint64_t result = rotl64(r->s[1] * 5, 7) * 9;
    dyn_prng_step(r);
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

/* ---------- jump / long_jump (2^128 / 2^192 advances) ---------- */

/* The xoshiro256** canonical jump tables (Vigna, prng.di.unimi.it,
 * xoshiro256starstar.c, public domain). Verified BOTH against the published
 * source and against the transition matrix built from dyn_prng_step by
 * scratch/probe_jump.c -- the first in-tree run of that probe REJECTED an
 * earlier transcription of JUMP[3] (0x...ac166f, a conflation), which is the
 * exact failure mode the matrix check exists to catch. */
static const uint64_t dyn_jump_table[4] = {
    0x180ec6d33cfd0abaULL, 0xd5a61266f0c9392cULL,
    0xa9582618e03fc9aaULL, 0x39abdc4529b1661cULL
};
static const uint64_t dyn_long_jump_table[4] = {
    0x76e15d3efefdcbbfULL, 0xc5004e441c522fb3ULL,
    0x77710069854ee241ULL, 0x39109bb02acbe635ULL
};

/* The state transition is GF(2)-linear, so advancing 2^k times is a fixed
 * linear map: for each set bit of the jump table, XOR the current state into
 * the accumulator, then step. 256 steps per jump. */
static void dyn_jump(dyn_prng_t *r, const uint64_t table[4])
{
    uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    int w, b;

    for (w = 0; w < 4; w++) {
        for (b = 0; b < 64; b++) {
            if ((table[w] >> b) & 1) {
                s0 ^= r->s[0];
                s1 ^= r->s[1];
                s2 ^= r->s[2];
                s3 ^= r->s[3];
            }
            dyn_prng_step(r);
        }
    }
    r->s[0] = s0;
    r->s[1] = s1;
    r->s[2] = s2;
    r->s[3] = s3;
}

void dyn_prng_jump(dyn_prng_t *r)
{
    dyn_jump(r, dyn_jump_table);
}

void dyn_prng_long_jump(dyn_prng_t *r)
{
    dyn_jump(r, dyn_long_jump_table);
}

/* ---------- distribution kernels (RD-1) ---------- */

/* See dyn-prng.h for the method contracts. Verified empirically against the
 * exact pmf / closed-form moments by scratch/probe_dist.c (chi-square +
 * mean/variance/sign-symmetry), which links THIS file, so the probe tests the
 * shipped code, not a copy. */
#include <math.h>

double dyn_prng_normal(dyn_prng_t *r, double mu, double sigma)
{
    for (;;) {
        double u = 2.0 * dyn_prng_next_double(r) - 1.0;   /* [-1, 1) */
        double v = 2.0 * dyn_prng_next_double(r) - 1.0;
        double s = u * u + v * v;
        if (s >= 1.0 || s == 0.0)
            continue;               /* outside the disk, or the degenerate center */
        return mu + sigma * (u * sqrt(-2.0 * log(s) / s));
    }
}

double dyn_prng_exponential(dyn_prng_t *r, double lambda)
{
    double u = dyn_prng_next_double(r);       /* [0, 1): 1-u in (0, 1], log(0) unreachable */
    return -log1p(-u) / lambda;
}

int64_t dyn_prng_poisson(dyn_prng_t *r, double lambda)
{
    double u;

    if (lambda <= 0.0)
        return 0;                    /* degenerate: all mass at 0, no draws */

    if (lambda < 30.0) {
        /* Knuth's product method: multiply uniforms until the product drops
         * under e^-lambda. e^-30 ~ 9.4e-14: no underflow below 30. */
        double L = exp(-lambda), p = 1.0;
        int64_t k = 0;
        do {
            k++;
            p *= dyn_prng_next_double(r);
        } while (p > L);
        return k - 1;
    }

    /* Centered inverse-CDF walk. One uniform; the CDF is walked from the
     * mode, so the expected work is O(sqrt(lambda)) either way. */
    u = dyn_prng_next_double(r);
    {
        int64_t m = (int64_t)floor(lambda);
        /* pmf(m) = e^-lambda * lambda^m / m! in log space; m+1 < 2^53 stays
         * exact, lgamma is the platform's (glibc/libm accuracy ~1 ulp). */
        double pm = exp(-lambda + (double)m * log(lambda)
                              - lgamma((double)m + 1.0));
        /* pass 1: total mass at or below the mode (tails below 1e-20 are
         * unreachable by a double uniform and folded). */
        double sum_down = pm, term = pm;
        int64_t k;
        for (k = m; k > 0; k--) {
            term = term * (double)k / lambda;      /* pmf(k-1) */
            if (term < 1e-20)
                break;
            sum_down += term;
        }
        if (u < sum_down) {
            /* the draw landed left of the mode. Walk down: D holds
             * CDF(k-1) = P(X <= k-1); the answer is the first k with
             * u >= CDF(k-1) (u < CDF(m) is guaranteed by the branch). */
            double D = sum_down - pm;          /* CDF(m-1) */
            term = pm;
            for (k = m; ; k--) {
                if (u >= D)
                    return k;
                if (k == 0)
                    return 0;      /* D == CDF(-1) == 0; u >= 0 always */
                term = term * (double)k / lambda;   /* pmf(k-1) */
                D -= term;                     /* now CDF(k-2) */
            }
        }
        /* right of the mode: accumulate the upper CDF until u is passed. */
        {
            double D = sum_down;
            term = pm;                       /* pmf(m) */
            for (k = m + 1; ; k++) {
                term = term * lambda / (double)k;   /* pmf(k) */
                D += term;
                if (u < D)
                    return k;
                if (term < 1e-20)
                    return k;      /* past the fold: beyond-double tail */
            }
        }
    }
}
