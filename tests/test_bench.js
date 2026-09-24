// tests/test_bench.js — dyna:bench (bench-only, slim-bench safe).
// flags:
import { bench } from "dyna:bench";
import * as bmod from "dyna:bench";

function assert(c, m) { if (!c) throw new Error("assert: " + m); }

// bench basic
{
  const r = bench("noop", () => {}, { timeMs: 30, warmupMs: 5 });
  assert(r.name === "noop", "name");
  assert(r.iters > 0, "iters");
  assert(r.opsPerSec > 0, "ops");
  assert(r.rsd >= 0, "rsd");
  assert(r.p50Ms >= 0 && r.p99Ms >= r.p50Ms, "p50/p99");
  assert(typeof r.meanMs === "number", "meanMs");
}
// warmup alias + defaults
{
  const r = bench("w", () => { let x = 0; for (let i = 0; i < 10; i++) x += i; return x; }, { timeMs: 20, warmup: 5 });
  assert(r.iters > 0, "warmup alias iters");
}
// strict opts
{
  let threw = false;
  try { bench("x", () => {}, { timeMs: 10, bogus: 1 }); } catch (e) { threw = /unknown option/.test(e.message); }
  assert(threw, "strict opts throw");
}
// table
{
  const t = bmod.table();
  assert(typeof t === "string" && t.includes("noop"), "table has noop");
}
// fn throw propagates
{
  let threw = false;
  try { bench("thrower", () => { throw new Error("boom"); }, { timeMs: 10, warmupMs: 0 }); } catch (e) { threw = /boom/.test(e.message); }
  assert(threw, "fn throw propagates");
}
print("test_bench ok");
