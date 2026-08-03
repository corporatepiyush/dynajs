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

import * as std from "std";

/** @typedef {{ name: string, fn: () => (void | Promise<void>) }} TestCase */

/** @type {TestCase[]} */
const registry = [];

/**
 * Register a test. `fn` may be sync or async; a throw marks the test failed.
 * @param {string} name
 * @param {() => (void | Promise<void>)} fn
 */
export function test(name, fn) {
  registry.push({ name, fn });
}

/**
 * Execute all registered tests in order, print a summary, and exit(0|1).
 * @param {string} title Human-readable suite name.
 * @returns {Promise<never>} never returns (calls std.exit).
 */
export async function run(title) {
  let passed = 0;
  const failures = [];
  const started = nowMs();

  for (const { name, fn } of registry) {
    try {
      await fn();
      passed++;
    } catch (err) {
      failures.push({ name, err });
    }
  }

  const elapsed = (nowMs() - started).toFixed(1);
  print(`\n=== ${title} ===`);
  print(`  ${passed}/${registry.length} tests passed in ${elapsed}ms`);
  for (const { name, err } of failures) {
    const msg = err && err.stack ? err.stack : String(err);
    print(`  ✗ ${name}\n      ${msg.split("\n").join("\n      ")}`);
  }

  const ok = failures.length === 0;
  print(ok ? "PASS" : "FAIL");
  std.exit(ok ? 0 : 1);
}

// --- assertions --------------------------------------------------------------

/** Assert a truthy condition. */
export function assert(cond, msg = "assertion failed") {
  if (!cond) throw new Error(msg);
}

/** Assert structural (deep) equality. */
export function assertEqual(actual, expected, msg = "") {
  if (!deepEqual(actual, expected)) {
    throw new Error(
      `${msg ? msg + ": " : ""}expected ${show(expected)}, got ${show(actual)}`,
    );
  }
}

/** Assert two numbers are within `eps` of each other. */
export function assertClose(actual, expected, eps = 1e-9, msg = "") {
  if (!(Math.abs(actual - expected) <= eps)) {
    throw new Error(
      `${msg ? msg + ": " : ""}expected ~${expected} (±${eps}), got ${actual}`,
    );
  }
}

/**
 * Assert that `fn` throws. If `match` is given, the error message must match
 * it (string → substring, RegExp → test).
 */
export function assertThrows(fn, match, msg = "expected an exception") {
  try {
    fn();
  } catch (err) {
    const em = err && err.message !== undefined ? String(err.message) : String(err);
    if (match !== undefined) {
      const ok = match instanceof RegExp ? match.test(em) : em.includes(match);
      if (!ok) throw new Error(`${msg}: got wrong error ${JSON.stringify(em)}`);
    }
    return;
  }
  throw new Error(msg);
}

// --- deep equality -----------------------------------------------------------

/**
 * Structural equality across primitives, arrays, plain objects, Map, Set,
 * typed arrays, ArrayBuffer, BigInt, and NaN. Cyclic structures are handled
 * via an identity-pair seen-set.
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

// --- misc --------------------------------------------------------------------

function nowMs() {
  // Prefer a monotonic-ish clock; fall back to Date.
  try {
    return globalThis.performance ? performance.now() : Date.now();
  } catch {
    return Date.now();
  }
}
