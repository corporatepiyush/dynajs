#include "dyn-prng.h"

#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <stdlib.h>
int dyn_os_entropy(void* buf, size_t n)
{
    arc4random_buf(buf, n);
    return 0;
}
#else
#include <sys/random.h>
#include <errno.h>
int dyn_os_entropy(void* buf, size_t n)
{
    uint8_t* p = (uint8_t*)buf;
    while (n > 0) {
        ssize_t got = getrandom(p, n, 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += got;
        n -= (size_t)got;
    }
    return 0;
}
#endif

static inline uint64_t rotl64(uint64_t x, int k)
{
    return (x << k) | (x >> (64 - k));
}

uint64_t dyn_splitmix64(uint64_t* x)
{
    uint64_t z = (*x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

void dyn_prng_seed(dyn_prng_t* r, uint64_t seed)
{
    uint64_t sm = seed;
    r->s[0] = dyn_splitmix64(&sm);
    r->s[1] = dyn_splitmix64(&sm);
    r->s[2] = dyn_splitmix64(&sm);
    r->s[3] = dyn_splitmix64(&sm);
}

uint64_t dyn_prng_next(dyn_prng_t* r)
{
    uint64_t result = rotl64(r->s[1] * 5, 7) * 9;
    dyn_prng_step(r);
    return result;
}

double dyn_prng_next_double(dyn_prng_t* r)
{
    return (double)(dyn_prng_next(r) >> 11) * (1.0 / 9007199254740992.0);
}

uint64_t dyn_prng_next_bounded(dyn_prng_t* r, uint64_t bound)
{
    uint64_t threshold;
    if (bound == 0)
        return 0;
    threshold = (0 - bound) % bound;
    for (;;) {
        uint64_t v = dyn_prng_next(r);
        if (v >= threshold)
            return v % bound;
    }
}

void dyn_prng_fill(dyn_prng_t* restrict r, uint8_t* restrict dst, size_t n)
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

int dyn_prng_seed_random(dyn_prng_t* r)
{
    uint64_t seed;
    if (dyn_os_entropy(&seed, sizeof(seed)) < 0)
        return -1;
    dyn_prng_seed(r, seed);
    return 0;
}

static const uint64_t dyn_jump_table[4] = {
    0x180ec6d33cfd0abaULL, 0xd5a61266f0c9392cULL,
    0xa9582618e03fc9aaULL, 0x39abdc4529b1661cULL
};
static const uint64_t dyn_long_jump_table[4] = {
    0x76e15d3efefdcbbfULL, 0xc5004e441c522fb3ULL,
    0x77710069854ee241ULL, 0x39109bb02acbe635ULL
};

static void dyn_jump(dyn_prng_t* r, const uint64_t table[4])
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

void dyn_prng_jump(dyn_prng_t* r)
{
    dyn_jump(r, dyn_jump_table);
}

void dyn_prng_long_jump(dyn_prng_t* r)
{
    dyn_jump(r, dyn_long_jump_table);
}

#include <math.h>

double dyn_prng_normal(dyn_prng_t* r, double mu, double sigma)
{
    for (;;) {
        double u = 2.0 * dyn_prng_next_double(r) - 1.0;
        double v = 2.0 * dyn_prng_next_double(r) - 1.0;
        double s = u * u + v * v;
        if (s >= 1.0 || s == 0.0)
            continue;
        return mu + sigma * (u * sqrt(-2.0 * log(s) / s));
    }
}

double dyn_prng_exponential(dyn_prng_t* r, double lambda)
{
    double u = dyn_prng_next_double(r);
    return -log1p(-u) / lambda;
}

int64_t dyn_prng_poisson(dyn_prng_t* r, double lambda)
{
    double u;

    if (lambda <= 0.0)
        return 0;

    if (lambda < 30.0) {
        double L = exp(-lambda), p = 1.0;
        int64_t k = 0;
        do {
            k++;
            p *= dyn_prng_next_double(r);
        } while (p > L);
        return k - 1;
    }

    u = dyn_prng_next_double(r);
    {
        int64_t m = (int64_t)floor(lambda);
        double pm = exp(-lambda + (double)m * log(lambda)
            - lgamma((double)m + 1.0));
        double sum_down = pm, term = pm;
        int64_t k;
        for (k = m; k > 0; k--) {
            term = term * (double)k / lambda;
            if (term < 1e-20)
                break;
            sum_down += term;
        }
        if (u < sum_down) {
            double D = sum_down - pm;
            term = pm;
            for (k = m;; k--) {
                if (u >= D)
                    return k;
                if (k == 0)
                    return 0;
                term = term * (double)k / lambda;
                D -= term;
            }
        }
        {
            double D = sum_down;
            term = pm;
            for (k = m + 1;; k++) {
                term = term * lambda / (double)k;
                D += term;
                if (u < D)
                    return k;
                if (term < 1e-20)
                    return k;
            }
        }
    }
}
