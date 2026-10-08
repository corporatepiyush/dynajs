#ifndef DYN_PRNG_H
#define DYN_PRNG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int dyn_os_entropy(void* buf, size_t n);

static inline uint64_t dyn_fastmod_M(uint64_t d)
{
    return UINT64_MAX / d + 1;
}

static inline uint64_t dyn_fastmod(uint64_t n, uint64_t M, uint64_t d)
{
    uint64_t r;
    if (d <= 1)
        return 0;
    r = n - (uint64_t)(((unsigned __int128)n * M) >> 64) * d;

    return (int64_t)r < 0 ? r + d : r;
}

uint64_t dyn_splitmix64(uint64_t* state);

typedef struct {
    uint64_t s[4];
} dyn_prng_t;

void dyn_prng_seed(dyn_prng_t* r, uint64_t seed);

int dyn_prng_seed_random(dyn_prng_t* r);

static inline void dyn_prng_step(dyn_prng_t* r)
{
    uint64_t* s = r->s;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = (s[3] << 45) | (s[3] >> (64 - 45));
}

uint64_t dyn_prng_next(dyn_prng_t* r);

double dyn_prng_next_double(dyn_prng_t* r);

uint64_t dyn_prng_next_bounded(dyn_prng_t* r, uint64_t bound);

void dyn_prng_fill(dyn_prng_t* restrict r, uint8_t* restrict dst, size_t n);

void dyn_prng_jump(dyn_prng_t* r);
void dyn_prng_long_jump(dyn_prng_t* r);

double dyn_prng_normal(dyn_prng_t* r, double mu, double sigma);

double dyn_prng_exponential(dyn_prng_t* r, double lambda);

int64_t dyn_prng_poisson(dyn_prng_t* r, double lambda);

#ifdef __cplusplus
}
#endif

#endif
