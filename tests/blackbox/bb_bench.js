// Parametric black-box contract test for dyna:bench, generated from dynajs.d.ts lines 6881-6900. Engine sources not consulted.
import * as bench from "dyna:bench";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertClose(actual, expected, eps, msg) { n++; if (!(Math.abs(actual - expected) <= eps)) throw new Error("assertion failed (close): " + msg + " — got |" + actual + "| expected |" + expected + "±" + eps + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
function eqArr(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }

let benchCalls = 0;
const lightWork = () => { let s = 0; for (let i = 0; i < 200; i++) s += i * 0.5; return s; };

// ============================ bench(): documented return shape + identities (d.ts L6882-6897) ============================
// "opsPerSec = 1000/meanMs; rsd = std/mean; p50Ms/p99Ms over per-iteration latencies." No exact timing values asserted.
{
  const r1 = bench.bench("bb_bench_light", lightWork, { timeMs: 10, warmupMs: 0 });
  benchCalls++;
  const shapeCases = [
    ["name passthrough", (r) => r.name === "bb_bench_light"],
    ["iters is an integer >= 1", (r) => Number.isInteger(r.iters) && r.iters >= 1],
    ["elapsedMs is a number >= 0", (r) => typeof r.elapsedMs === "number" && r.elapsedMs >= 0],
    ["meanMs is a number >= 0", (r) => typeof r.meanMs === "number" && r.meanMs >= 0],
    ["opsPerSec is a positive number", (r) => typeof r.opsPerSec === "number" && r.opsPerSec > 0],
    ["opsPerSec = 1000/meanMs", (r) => Math.abs(r.opsPerSec - 1000 / r.meanMs) <= Math.max(1e-6 * r.opsPerSec, 1e-9)],
    ["rsd = std/mean is >= 0", (r) => typeof r.rsd === "number" && r.rsd >= 0],
    ["p50Ms >= 0", (r) => typeof r.p50Ms === "number" && r.p50Ms >= 0],
    ["p99Ms >= 0", (r) => typeof r.p99Ms === "number" && r.p99Ms >= 0],
    ["p50Ms <= p99Ms (percentile order)", (r) => r.p50Ms <= r.p99Ms],
    ["all eight documented keys present", (r) => eqArr(["elapsedMs", "iters", "meanMs", "name", "opsPerSec", "p50Ms", "p99Ms", "rsd"].filter((k) => !(k in r)), [])],
  ];
  for (const [label, probe] of shapeCases) assert(probe(r1), label);

  // second run: shape stability + "Recorded for table()" (d.ts L6896)
  const r2 = bench.bench("bb_bench_second", lightWork, { timeMs: 10, warmup: 0 }); // "warmup aliases warmupMs"
  benchCalls++;
  for (const [label, probe] of [["second run has the same keys", (r) => eqArr(Object.keys(r).sort(), Object.keys(r1).sort())], ["second run name passthrough", (r) => r.name === "bb_bench_second"]]) assert(probe(r2), label);
}

// ============================ bench(): documented default options (d.ts L6894-6895) ============================
// "default 500, 1..60000" / "default 100, 0..60000" — one call with no opts exercises the defaults.
{
  const r = bench.bench("bb_bench_defaults", lightWork);
  benchCalls++;
  assert(Number.isInteger(r.iters) && r.iters >= 1, "default timeMs/warmupMs run produced at least one iteration");
  assert(r.meanMs >= 0, "default run meanMs >= 0");
}

// ============================ bench(): throwing fn propagates (d.ts L6896) ============================
{
  const cases = [
    ["throwing fn propagates TypeError", () => bench.bench("bb_bench_throws", () => { throw new TypeError("boom"); }, { timeMs: 5, warmupMs: 0 }), TypeError],
  ];
  for (const [label, fn, T] of cases) assertThrows(fn, label, T, /boom/);
}

// ============================ bench(): documented opts ranges (d.ts L6894-6895) ============================
// timeMs "1..60000", warmupMs "0..60000".
{
  const refusals = [
    ["timeMs 0 below 1..60000", () => bench.bench("t", lightWork, { timeMs: 0, warmupMs: 0 })],
    ["timeMs 60001 above 1..60000", () => bench.bench("t", lightWork, { timeMs: 60001, warmupMs: 0 })],
    ["warmupMs -1 below 0..60000", () => bench.bench("t", lightWork, { timeMs: 5, warmupMs: -1 })],
    ["warmupMs 60001 above 0..60000", () => bench.bench("t", lightWork, { timeMs: 5, warmupMs: 60001 })],
  ];
  for (const [label, fn] of refusals) assertThrows(fn, label);
}

// ============================ table(): header + one row per run (d.ts L6898-6899) ============================
// "Tab-separated table of all bench() results this process (header + one row per run)."
{
  const t = bench.table();
  assert(typeof t === "string", "table() returns a string");
  const lines = t.split("\n").filter((l) => l.length > 0);
  assert(lines.length >= benchCalls + 1, "table() has a header plus at least one row per bench() run");
  assert(lines[0].includes("\t"), "table() header is tab-separated");
  assert(lines.slice(1).some((l) => l.includes("\t")), "table() data rows are tab-separated");
  assert(t.includes("bb_bench_light"), "table() records the bench() runs by name");
}

print("bb_bench: all tests passed (" + n + " assertions)");
