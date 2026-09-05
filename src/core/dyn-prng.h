/*
 * dyn-prng -- xoshiro256** pseudo-random generation plus OS entropy. PURE C:
 * no JSValue, no JSContext. Compiles standalone with `-Isrc/core`.
 *
 * NOT CRYPTOGRAPHIC. xoshiro256** is fast, well-distributed and its state is
 * recoverable from its output; use it for simulation, sampling, shuffling and
 * test data. Anything security-bearing wants dyn_os_entropy(), which is the
 * kernel CSPRNG (arc4random_buf on Apple/BSD, getrandom on Linux).
 *
 * A dyn_prng_t is a compiled capability: seed it
 * once, draw from it indefinitely. It allocates nothing and lives wherever the
 * caller puts it, so a per-thread generator is a struct field, not a malloc.
 *
 * DETERMINISM IS A CONTRACT. A given seed reproduces a given stream exactly,
 * which is what makes dyna:ml's seeded fits reproducible. Do not change the
 * seeding schedule (SplitMix64 expansion) or the output function without
 * treating it as a breaking change to every recorded model and test vector.
 *
 * NOT thread-safe: one generator, one thread at a time. Concurrent draws want
 * one generator each -- which is cheap, precisely because there is no
 * allocation.
 */
#ifndef DYN_PRNG_H
#define DYN_PRNG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Kernel CSPRNG (arc4random_buf on Apple/BSD, getrandom on Linux). Returns 0
 * on success, -1 when the kernel source is unavailable: callers must fail
 * loudly (SP 800-90B 2.2.1/4.3: a failed entropy source produces no output).
 * Never degrades to a PRNG fallback. */
int dyn_os_entropy(void *buf, size_t n);

/* One SplitMix64 step: advances *state and returns the mixed value.
 *
 * Exposed because it is a generator in its own right, not just xoshiro's
 * seeding schedule. dyna:ml uses exactly this for k-means++ initialisation,
 * forest bagging and GBDT row subsampling, and its output stream IS the
 * reproducibility guarantee those fits advertise -- so it has to be the same
 * function, not an equivalent one. */
uint64_t dyn_splitmix64(uint64_t *state);

typedef struct {
    uint64_t s[4];
} dyn_prng_t;

/* Seed the four-word state by SplitMix64 expansion, as xoshiro's authors
 * specify. Any seed value is valid, including 0. */
void dyn_prng_seed(dyn_prng_t *r, uint64_t seed);

/* Seed from OS entropy (an unpredictable, unreproducible stream). -1 when
 * the kernel source is unavailable. */
int dyn_prng_seed_random(dyn_prng_t *r);

/* Raw 64-bit draw. */
uint64_t dyn_prng_next(dyn_prng_t *r);

/* Uniform in [0, 1) with 53 bits of mantissa precision. */
double dyn_prng_next_double(dyn_prng_t *r);

/* Uniform in [0, bound), unbiased by rejection -- no modulo bias. `bound` must
 * be non-zero. */
uint64_t dyn_prng_next_bounded(dyn_prng_t *r, uint64_t bound);

/* Fill `n` bytes from the generator. */
/* `dst` must not overlap the generator state -- filling a buffer with the
 * state it is generated from is nonsense, and without the promise the compiler
 * must reload s[] after every 8-byte store because a uint8_t* may alias
 * anything. That reload was the whole cost: 2.43 -> 6.30 GB/s. */
void dyn_prng_fill(dyn_prng_t *restrict r, uint8_t *restrict dst, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* DYN_PRNG_H */
