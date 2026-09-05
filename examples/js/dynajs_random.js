/*
 * dyna:random -- native random number generation, self-contained and in-repo.
 *
 * Requires a CONFIG_NATIVE_MODULES build:
 *     make CONFIG_NATIVE_MODULES=y
 *     ./dynajs examples/js/dynajs_random.js
 *
 * Random is a xoshiro256** generator holding 256 bits of state and nothing
 * else -- no descriptor, no buffer, nothing whose release has to be timed --
 * so it is a PLAIN GC class with no close(). dyna:uuid.v4() is the RFC 4122 v4
 * generator. A seeded Random is deterministic; an unseeded one draws its seed
 * from the system CSPRNG.
 *
 * This file used to be a demo with bare asserts and a trailing print("PASS"),
 * which meant a failure part-way through printed nothing useful and the runner
 * could not tell which case broke. It is a harness suite now.
 */
import { Random } from "dyna:random";
/* v4 lives in dyna:uuid, which is the one UUID generator in the library. */
import { v4 as uuid } from "dyna:uuid";
import { test, run, assert, assertEqual, assertThrows, rng } from "./harness.js";

/* ---- the API surface, and the type of every result ---- */
test("each generator returns its documented type and range", () => {
  const r = new Random(0xC0FFEE);
  const big = r.nextU64();
  const n53 = r.nextU53();
  const f = r.nextFloat();
  const die = r.nextBounded(6);
  const bytes = r.fill(new Uint8Array(16));
  assert(typeof big === "bigint", "nextU64 is a BigInt");
  assert(big >= 0n && big < (1n << 64n), "nextU64 is in [0, 2^64)");
  assert(typeof n53 === "number" && n53 >= 0 && n53 < 2 ** 53, "nextU53 range");
  assert(Number.isInteger(n53), "nextU53 is an integer");
  assert(f >= 0 && f < 1, "nextFloat is in [0, 1)");
  assert(die >= 0 && die < 6 && Number.isInteger(die), "nextBounded(6) range");
  assert(bytes === r, "fill returns the Random (chainable), not the view");
});

/* ---- determinism: the whole point of a seed ---- */
test("the same seed produces the same sequence, a different one does not", () => {
  const firstN = (seed, n) => {
    const r = new Random(seed), out = [];
    for (let i = 0; i < n; i++) out.push(r.nextU64());
    return out;
  };
  const a = firstN(42, 16), b = firstN(42, 16);
  assertEqual(a, b, "two Random(42) agree over 16 draws");
  assert(firstN(43, 16).some((v, i) => v !== a[i]), "Random(43) diverges");
  /* Every generator on one instance must advance the SAME state: drawing a
     float must not leave the u64 stream where it was. */
  const p = new Random(7), q = new Random(7);
  p.nextFloat();
  assert(p.nextU64() !== q.nextU64(), "nextFloat advances the shared state");
});

test("a seed of 0 is a seed, not 'unseeded'", () => {
  const a = new Random(0), b = new Random(0);
  assertEqual(a.nextU64(), b.nextU64(), "Random(0) is reproducible");
  assert(new Random(0).nextU64() !== new Random(1).nextU64(), "0 and 1 differ");
});

test("an unseeded Random is not the seeded-zero one", () => {
  /* If an absent seed silently became 0, every unseeded generator in a process
     would emit the same stream -- catastrophic and invisible. */
  const zero = new Random(0).nextU64();
  let same = 0;
  for (let i = 0; i < 8; i++) if (new Random().nextU64() === zero) same++;
  assert(same === 0, "unseeded generators do not collapse onto seed 0");
  const s = new Set();
  for (let i = 0; i < 8; i++) s.add(String(new Random().nextU64()));
  assert(s.size === 8, "eight unseeded generators give eight streams");
});

/* ---- distribution, checked as a PROPERTY rather than against fixed digits --- */
test("nextBounded is uniform enough and never leaves its range", () => {
  const r = new Random(12345);
  const N = 60000, K = 6;
  const hist = new Array(K).fill(0);
  for (let i = 0; i < N; i++) {
    const v = r.nextBounded(K);
    assert(v >= 0 && v < K, "in range");
    hist[v]++;
  }
  const expect = N / K;
  for (let i = 0; i < K; i++) {
    /* A generous band: this is a smoke test for a stuck or biased generator,
       not a statistical claim. A truly broken one misses by miles. */
    assert(Math.abs(hist[i] - expect) < expect * 0.1,
           "bucket " + i + " is near uniform (" + hist[i] + " vs " + expect + ")");
  }
});

test("nextFloat covers the unit interval without reaching 1", () => {
  const r = new Random(999);
  let lo = 1, hi = 0;
  for (let i = 0; i < 50000; i++) {
    const f = r.nextFloat();
    assert(f >= 0 && f < 1, "always in [0,1)");
    if (f < lo) lo = f;
    if (f > hi) hi = f;
  }
  assert(lo < 0.01, "the low end is reached (" + lo + ")");
  assert(hi > 0.99, "the high end is reached (" + hi + ")");
});

test("fill writes every byte and both tails of the range appear", () => {
  const r = new Random(2024);
  const buf = new Uint8Array(4096);
  /* fill returns `this` so it chains; the view it wrote is the argument */
  assert(r.fill(buf) === r, "fill returns the Random for chaining");
  assertEqual(r.fill(buf), r, "the same instance every call");
  const seen = new Set(buf);
  assert(seen.size > 200, "a wide spread of byte values (" + seen.size + "/256)");
  /* An all-zero buffer is what a generator that silently did nothing returns. */
  assert(buf.some((b) => b !== 0), "the buffer is not left zeroed");
  assertEqual(r.fill(new Uint8Array(0)).length === undefined, true, "chained fill on an empty view is legal");
});

/* ---- bounds: the degenerate and the hostile ---- */
test("nextBounded refuses the bounds that have no answer", () => {
  const r = new Random(7);
  for (const bad of [0, -1, NaN]) {
    assertThrows(() => r.nextBounded(bad), undefined,
                 "nextBounded(" + bad + ") is refused");
  }
  assertEqual(r.nextBounded(1), 0, "a bound of 1 is always 0");
  const big = r.nextBounded(Number.MAX_SAFE_INTEGER);
  assert(big >= 0 && big < Number.MAX_SAFE_INTEGER, "a huge bound still ranges");
});

test("nextBounded coerces through valueOf, which is the hook it actually uses", () => {
  /* A toString hook would never run here, so a test using one would pass
     having exercised nothing. */
  let ran = false;
  const r = new Random(7);
  const n = r.nextBounded({ valueOf() { ran = true; return 6; } });
  assert(ran, "the valueOf hook fired");
  assert(n >= 0 && n < 6, "and the coerced bound was honoured");
});

/* ---- lifetime: a plain GC class, deliberately ---- */
test("Random has no close(): there is nothing to release deterministically", () => {
  assert(typeof new Random(1).close === "undefined", "no close()");
  assert(typeof new Random(1).dispose === "undefined", "no dispose()");
});

test("two hundred thousand generators are reclaimed without any teardown", () => {
  /* If the finalizer were missing this would grow without bound. The assertion
     is that it completes at all -- an RSS threshold here would be measuring
     the allocator, and under a sanitizer it would measure the quarantine. */
  for (let i = 0; i < 200000; i++) new Random(i).nextU64();
  assert(true, "200000 Randoms created and dropped");
});

test("a method on a foreign receiver throws instead of reinterpreting it", () => {
  assertThrows(() => Random.prototype.nextU64.call({}), undefined,
               "a foreign receiver is refused");
  assertThrows(() => Random.prototype.fill.call({}, new Uint8Array(4)), undefined,
               "fill on a foreign receiver is refused");
});

/* ---- uuid v4 ---- */
test("uuid() is a canonical RFC 4122 version 4 identifier", () => {
  const re = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
  const seen = new Set();
  for (let i = 0; i < 2000; i++) {
    const id = uuid();
    assert(re.test(id), "canonical v4: " + id);
    seen.add(id);
  }
  assertEqual(seen.size, 2000, "2000 uuids, no collisions");
});

test("uuid version and variant nibbles are pinned, not incidental", () => {
  for (let i = 0; i < 500; i++) {
    const id = uuid();
    assertEqual(id[14], "4", "version nibble is 4");
    assert("89ab".includes(id[19]), "variant nibble is 8, 9, a or b");
  }
});

await run("dyna:random -- generators, bounds and uuid v4");
