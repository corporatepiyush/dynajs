// harness.js — a tiny, dependency-free test harness shared by every example.
//
// Design: a *deep module* — small surface (test/run/assert*/deepEqual/rng),
// large implementation. Each example registers named tests with `test(...)`,
// then calls `await run(title)`. `run` prints a per-suite summary line ending
// in exactly "PASS" or "FAIL" and exits the process with 0 or 1 so the suite
// can be scripted (see run_all.js).
//
// Runs on the dynascript `dynajs` (DynaJS-ng fork). The only platform dependency
// is `std.exit`, imported below; everything else is pure ECMAScript.
//
// THREE PROPERTIES THIS HARNESS ENFORCES, each paid for by a real defect:
//
//   1. A SKIP ANNOUNCES ITSELF AND IS COUNTED. A conditional guard around a
//      check turns a missed precondition into a lower pass count with zero
//      failures, which reads as green — and nobody compares counts between
//      runs. `skip()` prints, and `run` reports the tally.
//   2. A TEST THAT ASSERTS NOTHING IS NOT A PASS. Every assertion increments a
//      counter; a test that finishes having made none is reported, because a
//      test that cannot fail has told you nothing.
//   3. EVERY CASE HAS A TIME BOUND, and one bound is not enough. An async test
//      races a timer here. A synchronous spin cannot be preempted from inside
//      a single-threaded engine, so that half is bounded by run_all.js at the
//      PROCESS level (CPU rlimit + wall clock). Neither alone is sufficient:
//      a spin burns CPU, a blocked read burns none.

import * as std from "std";

/** @typedef {{ name: string, fn: () => (void|Promise<void>), timeoutMs: number }} TestCase */

/** @type {TestCase[]} */
const registry = [];
/** @type {{name: string, reason: string}[]} */
const skipped = [];

/** Assertions executed, total and within the running test. */
let assertionCount = 0;
let assertionsHere = 0;

/** Default per-test bound. Generous: it exists to catch a hang, not to time. */
export const DEFAULT_TIMEOUT_MS = 20000;

/**
 * Register a test. `fn` may be sync or async; a throw marks the test failed.
 * @param {string} name
 * @param {() => (void | Promise<void>)} fn
 * @param {{timeoutMs?: number}} [opts]
 */
export function test(name, fn, opts) {
  registry.push({
    name,
    fn,
    timeoutMs: (opts && opts.timeoutMs) || DEFAULT_TIMEOUT_MS,
  });
}

/**
 * Record a test that will NOT run, and say so out loud. Use this instead of an
 * early `return` inside a test: a silent skip is indistinguishable from a pass.
 * @param {string} name
 * @param {string} reason why — be specific, "unavailable" helps nobody.
 */
export function skip(name, reason) {
  skipped.push({ name, reason });
}

/**
 * Run `fn` only when `cond` holds; otherwise register an announced skip.
 * The reason is required, so a skip can never be silent.
 */
export function testIf(cond, name, reason, fn, opts) {
  if (cond) test(name, fn, opts);
  else skip(name, reason);
}

/** Race a promise against a timer so a hung await cannot hang the suite. */
function withTimeout(p, ms, name) {
  let timer;
  const bomb = new Promise((_, reject) => {
    timer = setTimeout(
      () => reject(new Error(`TIMEOUT after ${ms}ms (async): ${name}`)),
      ms,
    );
  });
  return Promise.race([p, bomb]).finally(() => clearTimeout(timer));
}

/**
 * Execute all registered tests in order, print a summary, and exit(0|1).
 * @param {string} title Human-readable suite name.
 * @returns {Promise<never>} never returns (calls std.exit).
 */
export async function run(title) {
  let passed = 0;
  const failures = [];
  const vacuous = [];
  const slow = [];
  const started = nowMs();

  for (const { name, fn, timeoutMs } of registry) {
    assertionsHere = 0;
    const t0 = nowMs();
    try {
      const r = fn();
      // Only await (and only arm the timer) when there is actually a promise:
      // wrapping a sync test would cost a microtask turn per case.
      if (r && typeof r.then === "function") await withTimeout(r, timeoutMs, name);
      passed++;
      if (assertionsHere === 0) vacuous.push(name);
    } catch (err) {
      failures.push({ name, err });
    }
    const dt = nowMs() - t0;
    if (dt > timeoutMs / 2) slow.push({ name, dt });
  }

  const elapsed = (nowMs() - started).toFixed(1);
  print(`\n=== ${title} ===`);
  print(
    `  ${passed}/${registry.length} tests passed in ${elapsed}ms` +
      `  (${assertionCount} assertions` +
      (skipped.length ? `, ${skipped.length} skipped` : "") + `)`,
  );
  for (const { name, reason } of skipped) print(`  ○ SKIP ${name} — ${reason}`);
  // A test that asserted nothing is reported but does NOT fail the suite: some
  // examples legitimately just have to not throw. Saying so is the point.
  for (const name of vacuous) print(`  ! ${name} made no assertions`);
  for (const { name, dt } of slow) {
    print(`  ! ${name} took ${dt.toFixed(0)}ms, over half its ${DEFAULT_TIMEOUT_MS}ms bound`);
  }
  for (const { name, err } of failures) {
    // MESSAGE first, then the frames. This engine's `.stack` carries only the
    // frames, so reporting it alone dropped every "expected X, got Y" -- the
    // one part of a failure that says what went wrong.
    const message = err && err.message !== undefined ? String(err.message) : String(err);
    const frames = err && err.stack ? String(err.stack) : "";
    const body = frames ? message + "\n" + frames : message;
    print(`  ✗ ${name}\n      ${body.split("\n").join("\n      ")}`);
  }

  const ok = failures.length === 0;
  print(ok ? "PASS" : "FAIL");
  std.exit(ok ? 0 : 1);
}

// --- assertions --------------------------------------------------------------

function tick() { assertionCount++; assertionsHere++; }

/** Assert a truthy condition. */
export function assert(cond, msg = "assertion failed") {
  tick();
  if (!cond) throw new Error(msg);
}

/** Fail unconditionally — for the "this line must be unreachable" case. */
export function fail(msg = "unreachable") {
  tick();
  throw new Error(msg);
}

/** Assert structural (deep) equality. */
export function assertEqual(actual, expected, msg = "") {
  tick();
  if (!deepEqual(actual, expected)) {
    throw new Error(
      `${msg ? msg + ": " : ""}expected ${show(expected)}, got ${show(actual)}`,
    );
  }
}

/** Assert SAME-VALUE equality (Object.is): distinguishes -0 from +0, NaN from NaN. */
export function assertIs(actual, expected, msg = "") {
  tick();
  if (!Object.is(actual, expected)) {
    throw new Error(
      `${msg ? msg + ": " : ""}expected Object.is ${show(expected)}, got ${show(actual)}`,
    );
  }
}

/** Assert two numbers are within `eps` of each other. */
export function assertClose(actual, expected, eps = 1e-9, msg = "") {
  tick();
  if (!(Math.abs(actual - expected) <= eps)) {
    throw new Error(
      `${msg ? msg + ": " : ""}expected ~${expected} (±${eps}), got ${actual}`,
    );
  }
}

/**
 * Assert two floats agree to within `ulps` units in the last place. Across
 * precisions or reassociated sums, bit-equality is the wrong assertion: the
 * same source rounds differently where the ISA has an FMA.
 */
export function assertUlp(actual, expected, ulps = 4, msg = "") {
  tick();
  if (Object.is(actual, expected)) return;
  if (!Number.isFinite(actual) || !Number.isFinite(expected)) {
    throw new Error(`${msg ? msg + ": " : ""}non-finite: ${actual} vs ${expected}`);
  }
  const scale = Math.max(Math.abs(actual), Math.abs(expected));
  const eps = Math.pow(2, Math.floor(Math.log2(scale || Number.MIN_VALUE)) - 52);
  if (Math.abs(actual - expected) > ulps * eps) {
    throw new Error(
      `${msg ? msg + ": " : ""}expected ${expected} within ${ulps} ulp, got ${actual}`,
    );
  }
}

/**
 * Assert that `fn` throws. If `match` is given, the error message must match
 * it (string → substring, RegExp → test).
 *
 * Matching the MESSAGE matters when two guards catch one symptom: without it,
 * deleting the first guard leaves the second throwing and the test still green,
 * so the assertion proves the behaviour and nothing about the code.
 */
export function assertThrows(fn, match, msg = "expected an exception") {
  tick();
  try {
    fn();
  } catch (err) {
    const em = err && err.message !== undefined ? String(err.message) : String(err);
    if (match !== undefined) {
      const ok = match instanceof RegExp ? match.test(em) : em.includes(match);
      if (!ok) throw new Error(`${msg}: got wrong error ${JSON.stringify(em)}`);
    }
    return err;
  }
  throw new Error(msg);
}

/** The async twin of assertThrows. Accepts a promise or a function returning one. */
export async function assertRejects(p, match, msg = "expected a rejection") {
  tick();
  try {
    await (typeof p === "function" ? p() : p);
  } catch (err) {
    const em = err && err.message !== undefined ? String(err.message) : String(err);
    if (match !== undefined) {
      const ok = match instanceof RegExp ? match.test(em) : em.includes(match);
      if (!ok) throw new Error(`${msg}: got wrong error ${JSON.stringify(em)}`);
    }
    return err;
  }
  throw new Error(msg);
}

// --- deep equality -----------------------------------------------------------

/**
 * Structural equality across primitives, arrays, plain objects, Map, Set,
 * typed arrays, ArrayBuffer, Date, RegExp, BigInt, and NaN. Cyclic structures
 * are handled via an identity-pair seen-set.
 *
 * Note -0 and +0 compare EQUAL here (=== semantics), which is usually what a
 * structural comparison wants; use assertIs when the sign of zero is the point.
 */
export function deepEqual(a, b, seen = new Set()) {
  if (a === b) return true; // fast path incl. same reference
  if (typeof a === "number" && typeof b === "number") {
    return a !== a && b !== b; // NaN === NaN for test purposes
  }
  if (typeof a !== "object" || typeof b !== "object" || a === null || b === null) {
    return false;
  }

  // Guard against cycles: key on the ordered pair identity.
  const key = pairKey(a, b);
  if (seen.has(key)) return true;
  seen.add(key);

  if (a instanceof Date || b instanceof Date) {
    return a instanceof Date && b instanceof Date && Object.is(a.getTime(), b.getTime());
  }
  if (a instanceof RegExp || b instanceof RegExp) {
    return a instanceof RegExp && b instanceof RegExp &&
      a.source === b.source && a.flags === b.flags;
  }
  if (ArrayBuffer.isView(a) || ArrayBuffer.isView(b)) return typedEqual(a, b);
  if (a instanceof ArrayBuffer || b instanceof ArrayBuffer) {
    return a instanceof ArrayBuffer && b instanceof ArrayBuffer &&
      typedEqual(new Uint8Array(a), new Uint8Array(b));
  }
  if (a instanceof Map || b instanceof Map) return mapEqual(a, b, seen);
  if (a instanceof Set || b instanceof Set) return setEqual(a, b);

  const aArr = Array.isArray(a);
  const bArr = Array.isArray(b);
  if (aArr !== bArr) return false;
  if (aArr) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (!deepEqual(a[i], b[i], seen)) return false;
    return true;
  }

  const ak = Object.keys(a);
  const bk = Object.keys(b);
  if (ak.length !== bk.length) return false;
  for (const k of ak) {
    if (!Object.hasOwn(b, k)) return false;
    if (!deepEqual(a[k], b[k], seen)) return false;
  }
  return true;
}

function pairKey(a, b) {
  // A cheap, allocation-light identity marker for the cycle guard.
  return a === a ? `${objId(a)}|${objId(b)}` : "nan";
}

let _idSeq = 0;
const _ids = new WeakMap();
function objId(o) {
  if (typeof o !== "object" || o === null) return String(o);
  let id = _ids.get(o);
  if (id === undefined) _ids.set(o, (id = ++_idSeq));
  return id;
}

function typedEqual(a, b) {
  if (!ArrayBuffer.isView(a) || !ArrayBuffer.isView(b)) return false;
  if (a.constructor !== b.constructor || a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

function mapEqual(a, b, seen) {
  if (!(a instanceof Map) || !(b instanceof Map) || a.size !== b.size) return false;
  for (const [k, v] of a) {
    if (!b.has(k)) return false;
    if (!deepEqual(v, b.get(k), seen)) return false;
  }
  return true;
}

function setEqual(a, b) {
  if (!(a instanceof Set) || !(b instanceof Set) || a.size !== b.size) return false;
  for (const v of a) if (!b.has(v)) return false;
  return true;
}

// --- pretty printing ---------------------------------------------------------

/** Human-friendly, BigInt/Map/Set/TypedArray-aware value formatter. */
export function show(x, depth = 0) {
  if (depth > 4) return "…";
  switch (typeof x) {
    case "string": return JSON.stringify(x);
    case "bigint": return `${x}n`;
    case "function": return `[fn ${x.name || "anonymous"}]`;
    case "symbol": return x.toString();
    case "undefined": return "undefined";
  }
  if (x === null) return "null";
  if (Array.isArray(x)) return `[${x.map((v) => show(v, depth + 1)).join(", ")}]`;
  if (x instanceof Date) return `Date(${x.toISOString()})`;
  if (x instanceof RegExp) return String(x);
  if (ArrayBuffer.isView(x)) return `${x.constructor.name}(${Array.from(x).join(", ")})`;
  if (x instanceof Map) {
    return `Map{${[...x].map(([k, v]) => `${show(k, depth + 1)} => ${show(v, depth + 1)}`).join(", ")}}`;
  }
  if (x instanceof Set) return `Set{${[...x].map((v) => show(v, depth + 1)).join(", ")}}`;
  if (typeof x === "object") {
    return `{${Object.entries(x).map(([k, v]) => `${k}: ${show(v, depth + 1)}`).join(", ")}}`;
  }
  return String(x);
}

// --- deterministic PRNG (for property-based tests) ---------------------------

/**
 * mulberry32 — a fast, seedable 32-bit PRNG. Deterministic given a seed, so
 * property-based tests are reproducible across runs and engines.
 * @param {number} seed
 * @returns {() => number} generator returning floats in [0, 1).
 */
export function rng(seed = 0x2545f491) {
  let s = seed >>> 0;
  return function next() {
    s |= 0;
    s = (s + 0x6d2b79f5) | 0;
    let t = Math.imul(s ^ (s >>> 15), 1 | s);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/** Uniform integer in [lo, hi] from a `rng()` generator. */
export function randInt(next, lo, hi) {
  return lo + Math.floor(next() * (hi - lo + 1));
}

/**
 * Check `prop` over `n` generated inputs, and SHRINK a failure before
 * reporting it. "failed on a 4096-char string inside a 3-deep object" is
 * unactionable; the minimal case is the bug report.
 *
 * `shrinkOf` yields simpler candidates for a failing input; the first that
 * still fails replaces it, repeatedly, bounded so shrinking cannot outrun the
 * search it came from.
 */
export function forAll(gen, prop, opts = {}) {
  const n = opts.n || 200;
  const seed = opts.seed || 0x2545f491;
  const next = rng(seed);
  const shrinkOf = opts.shrink;
  const failed = (x) => {
    try { prop(x); return false; } catch (e) { return e; }
  };
  for (let i = 0; i < n; i++) {
    const x = gen(next, i);
    const err = failed(x);
    if (!err) continue;
    let best = x, bestErr = err, steps = 0;
    while (shrinkOf && steps < 200) {
      let improved = false;
      for (const c of shrinkOf(best)) {
        const e2 = failed(c);
        // The shrink predicate must test the SAME symptom, or it walks toward
        // a different bug and reports the minimal case for that one.
        if (e2 && String(e2.message) === String(bestErr.message)) {
          best = c; bestErr = e2; improved = true; steps++;
          break;
        }
      }
      if (!improved) break;
    }
    tick();
    throw new Error(
      `property failed after ${i + 1} cases (seed ${seed}); minimal input ` +
      `${show(best)}: ${bestErr.message}`,
    );
  }
  tick();
}

/** Shrinkers for the common generated shapes, for use with forAll. */
export const shrink = {
  int: (x) => {
    const out = [];
    if (x !== 0) out.push(0);
    if (x > 1) out.push(Math.floor(x / 2), x - 1);
    if (x < -1) out.push(Math.ceil(x / 2), x + 1);
    return out;
  },
  string: (s) => {
    const out = [];
    if (s.length > 0) out.push("");
    if (s.length > 1) {
      out.push(s.slice(0, Math.floor(s.length / 2)));
      out.push(s.slice(Math.floor(s.length / 2)));
      out.push(s.slice(1), s.slice(0, -1));
    }
    return out;
  },
  array: (a) => {
    const out = [];
    if (a.length > 0) out.push([]);
    if (a.length > 1) {
      out.push(a.slice(0, Math.floor(a.length / 2)));
      out.push(a.slice(Math.floor(a.length / 2)));
      out.push(a.slice(1), a.slice(0, -1));
    }
    return out;
  },
};

// --- misc --------------------------------------------------------------------

function nowMs() {
  // Prefer a monotonic-ish clock; fall back to Date.
  try {
    return globalThis.performance ? performance.now() : Date.now();
  } catch {
    return Date.now();
  }
}
