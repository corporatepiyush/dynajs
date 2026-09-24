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

/* Fast remainder by multiply-high (the "fastmod" construction): given
 * M = dyn_fastmod_M(d), dyn_fastmod(n, M, d) == n % d EXACTLY for every n in
 * [0, 2^64) and every d in [2, 2^63). The multiply-high quotient is
 * floor(n/d) or floor(n/d)+1, so one conditional add-back restores the true
 * remainder; d == 1 is the caller's special case (the remainder is 0) because
 * M overflows. Validated exhaustively/randomly over 1.5e8 adversarial (n, d)
 * pairs including 2^64-1 and every power-of-two boundary. For a runtime
 * divisor this replaces a 64-bit division per draw with a multiply-high plus
 * a multiply -- the per-draw cost of ml's bootstrap sampler. */
static inline uint64_t dyn_fastmod_M(uint64_t d)
{
    return UINT64_MAX / d + 1;
}

static inline uint64_t dyn_fastmod(uint64_t n, uint64_t M, uint64_t d)
{
    uint64_t r = n - (uint64_t)(((unsigned __int128)n * M) >> 64) * d;

    return (int64_t)r < 0 ? r + d : r;
}

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

/* One xoshiro256** state transition (the half of dyn_prng_next that updates
 * s[], with no output computation). Static inline so the hot draw path pays
 * nothing. Exposed because the jump constants below are DRIVEN by this exact
 * transition: the published jump table is checked against the matrix
 * T^(2^128) built from this function by scratch/probe_jump.c -- a second
 * implementation of the step here would let the two drift apart silently. */
static inline void dyn_prng_step(dyn_prng_t *r)
{
    uint64_t *s = r->s;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = (s[3] << 45) | (s[3] >> (64 - 45));
}

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

/* Advance the state by 2^128 / 2^192 draws without generating them: the
 * xoshiro256** canonical jump() / long_jump() (Vigna, prng.di.unimi.it).
 * Two generators seeded alike and then separated by one jump produce
 * disjoint output subsequences -- independent parallel streams for workers
 * or Monte Carlo lanes at the cost of 256 transition steps.
 *
 * The constants are NOT trusted on authority: scratch/probe_jump.c rebuilds
 * the 256x256 GF(2) transition matrix from dyn_prng_step above, computes
 * T^(2^128) / T^(2^192) by repeated squaring, and checks the table-driven
 * jump reproduces it exactly for 1000 random seeds. A wrong constant in this
 * table fails that probe loudly. */
void dyn_prng_jump(dyn_prng_t *r);
void dyn_prng_long_jump(dyn_prng_t *r);

/* ---------- distribution kernels (RD-1) ---------- */

/* Every kernel draws from the SAME instance stream, in stream order, so a
 * seeded generator reproduces its distribution sequences exactly. Argument
 * validation lives at the JS boundary; these kernels state their assumption. */

/* Normal(mu, sigma) by Marsaglia's polar method: rejection on the unit disk
 * (no trig), sqrt+log once per accepted PAIR. The second normal of each pair
 * is deliberately NOT cached: the native Random object stays a bare
 * dyn_prng_t, so getState()/setState() keep their exact documented 32-byte
 * format and a mid-stream checkpoint replays a normal() sequence losslessly.
 * Assumes sigma >= 0 and both finite. */
double dyn_prng_normal(dyn_prng_t *r, double mu, double sigma);

/* Exponential(lambda) by inverse CDF: -log1p(-u)/lambda, u in [0, 1), so the
 * log's argument lies in (0, 1] and never underflows the log. Assumes
 * lambda > 0, finite. */
double dyn_prng_exponential(dyn_prng_t *r, double lambda);

/* Poisson(lambda): exact integer in [0, lambda + 9 sqrt(lambda) + 64].
 * lambda < 30: Knuth's product method (e^-30 ~ 9.4e-14, safe in double).
 * lambda >= 30: centered inverse-CDF walk in O(sqrt(lambda)) with ONE
 *   uniform draw: pmf at the mode from libm lgamma (a platform primitive,
 *   no hand-copied tables), then pmf(k-1) = pmf(k)*k/lambda down and
 *   pmf(k+1) = pmf(k)*lambda/(k+1) up. Tails below 1e-20 absolute mass are
 *   folded to the walk boundary -- under the 2^-53 granularity of the
 *   uniform, so no reachable draw changes outcome. The upper walk also ends
 *   there, so the function terminates for every u in [0, 1).
 * Assumes 0 <= lambda < 2^31 (the boundary refuses larger). */
int64_t dyn_prng_poisson(dyn_prng_t *r, double lambda);

#ifdef __cplusplus
}
#endif

#endif /* DYN_PRNG_H */
