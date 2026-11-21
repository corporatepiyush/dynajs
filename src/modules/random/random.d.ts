/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:random
/** dyna:random — a seedable xoshiro256** PRNG with distributions, shuffle/sample and checkpointable state; NOT for secrets (use crypto.RandomBytes). */
declare module "dyna:random" {
    /** A seedable xoshiro256** PRNG; a given seed is deterministic and reproducible. */
    class Random {
        /** The seed coerces via ToInt64 ("42" and 42n give the 42 stream); omitted draws from OS entropy. */
        constructor(seed?: number | bigint);
        /** A full 64-bit draw, always BigInt. */
        nextU64(): bigint;
        /** The top 53 bits as an exact Number in [0, 2^53). */
        nextU53(): number;
        /** A double in [0, 1). */
        nextFloat(): number;
        /** Uniform in [0, bound) by rejection sampling (no modulo bias); the result type mirrors the argument; a Number bound must be an integer in [1, 2^53]. */
        nextBounded(bound: number): number;
        nextBounded(bound: bigint): bigint;
        /** Fills any typed array with random bytes over an optional (offset, length) window in ELEMENTS; returns `this`; DataView/ArrayBuffer are refused. */
        fill(typedArray: AnyTypedArray, offset?: number, length?: number): this;
        /** Fills the whole array with N(mu, sigma) draws (both values of each polar pair are used); f32 lanes are rounded once. */
        fillNormal(buf: Float32Array | Float64Array, mu?: number, sigma?: number): this;
        /** Fills with uniform draws in [lo, hi) (default [0, 1)); a float32 value that rounds up to hi is stepped back below it. */
        fillUniform(buf: Float32Array | Float64Array, lo?: number, hi?: number): this;
        /** Fills with exponential draws of rate lambda (default 1), by inversion as exponential() does. */
        fillExponential(buf: Float32Array | Float64Array, lambda?: number): this;
        /** n fresh random bytes; n must be an integer in [0, 2^30] (RangeError otherwise, before any allocation or draw).
         *  The keystream is filled straight into the result's engine-heap buffer (no scratch copy: peak memory is n, not 2n; ~5 GB/s fill), so the result counts toward the engine memory limit, not setNativeMemoryLimit. */
        bytes(n: number): Uint8Array;
        /** Normal(mu = 0, sigma = 1) by Marsaglia's polar method with no cached second draw, so state checkpoints replay exactly. */
        normal(mu?: number, sigma?: number): number;
        /** Exponential(lambda = 1) by inverse CDF; lambda must be finite and > 0. */
        exponential(lambda?: number): number;
        /** Exact Poisson(lambda) for lambda in [0, 2^31-1]; returns a non-negative integer. */
        poisson(lambda: number): number;
        /** Gamma(shape, scale = 1) by Marsaglia-Tsang (shape < 1 boosted by U^(1/shape)); both must be finite and > 0. */
        gamma(shape: number, scale?: number): number;
        /** Beta(a, b) in [0, 1] as X / (X + Y) of two gamma draws (in log space when a shape is below 1, so tiny shapes give 0 or 1, never NaN). */
        beta(a: number, b: number): number;
        /** Exact Binomial(n, p) by inversion searched outward from the mode (one uniform per draw); n is an integer in [0, 2^31-1], p in [0, 1]. */
        binomial(n: number, p: number): number;
        /** In-place Fisher-Yates from THIS instance's stream (so it is reproducible); returns `this`; frozen collections are refused. */
        shuffle(arr: AnyTypedArray | unknown[]): this;
        /** n elements WITHOUT replacement; the source is not mutated; a typed array yields its own type. */
        sample(arr: AnyTypedArray | unknown[], n: number): AnyTypedArray | unknown[];
        /** One uniform element; an empty array is a RangeError. */
        choice(arr: AnyTypedArray | unknown[]): unknown;
        /** Advances the state by 2^128 draws without generating them: same-seed generators separated by a jump give disjoint streams. */
        jump(): this;
        /** longJump: the 2^192-advance variant, for 2^64 independent lanes. */
        longJump(): this;
        /** A 32-byte opaque snapshot of the generator state (four LE u64 words). */
        getState(): Uint8Array;
        /** Restores a state captured by getState(); an all-zero state is the xoshiro fixed point and is refused. */
        setState(state: Uint8Array): void;
    }
}
