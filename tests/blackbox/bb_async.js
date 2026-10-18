// Parametric black-box contract test, generated from dynajs.d.ts lines 7983-8086. Engine sources not consulted.
// Coverage: dyna:async -- sleep, withTimeout, retry, Semaphore, Queue, Channel, pmap, Pool, debounce, throttle.
// Doc honesty notes honored: nothing here is PARALLEL (concurrency-limited interleaving only), and argument
// refusals throw SYNCHRONOUSLY (assertThrows proves sync-ness: a returned rejected promise would not throw).
//
// Table convention: every row is [label, () => actual (may be async), expectation] where expectation is
//   - an exact value -> assertEq (NaN-safe), an array -> deep equality via JSON,
//   - a predicate fn -> structural fact (row comment says which),
//   - {throws: patternOrNull} -> must throw synchronously (sync tables only).
// One loop per table; failure messages name the row label. All timing uses generous slack (assert counts and
// ordering, never exact times); total sleeps in this file stay under 500ms.

import * as A from "dyna:async";
const { withTimeout, retry, Semaphore, Queue, Channel, pmap, Pool, debounce, throttle } = A;

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type: " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }

async function runRowsAsync(rows) {
  for (const [label, get, exp] of rows) {
    const actual = await get();
    if (typeof exp === "function") { assert(exp(actual), label + " [structural predicate]"); continue; }
    if (Array.isArray(exp)) { assertDeepEq(actual, exp, label); continue; }
    assertEq(actual, exp, label);
  }
}
function runRows(rows) {
  for (const [label, get, exp] of rows) {
    if (exp !== null && typeof exp === "object" && "throws" in exp) { assertThrows(get, label, null, exp.throws); continue; }
    const actual = get();
    if (typeof exp === "function") { assert(exp(actual), label + " [structural predicate]"); continue; }
    if (Array.isArray(exp)) { assertDeepEq(actual, exp, label); continue; }
    assertEq(actual, exp, label);
  }
}
const never = new Promise(() => {});
const outcome = (p) => p.then(() => "resolved", (e) => (e && e.message ? e.message : String(e)));

// ---------------------------------------------------------------------------
// withTimeout (doc lines 7997-8001): rejects if p does not settle within ms;
// resolves/rejects with p otherwise; reason replaces the message; the late
// underlying rejection is handled even when the timer wins.
// ---------------------------------------------------------------------------
await runRowsAsync([
  ["fast promise passes its value through", () => withTimeout(sleep(2).then(() => "ok"), 200), "ok"],
  ["fast rejection propagates the original error (doc: rejects with p otherwise)", () => withTimeout(Promise.reject(new Error("boom")), 200).then(() => "resolved", (e) => e.message), "boom"],
  // no `reason` passed: the doc pins nothing about the DEFAULT message, only
  // that the promise rejects at the deadline (and that a passed `reason`
  // replaces the message) — so pin rejection with a non-empty message
  ["slow promise rejects at the deadline", () => outcome(withTimeout(never, 8)).then((s) => s !== "resolved" && s.length > 0), true],
  ["custom reason replaces the message (structural: containment, not exact wording)", () => outcome(withTimeout(never, 8, "too slow")).then((s) => s.includes("too slow")), true],
  ["late underlying rejection does NOT surface after the timer wins (doc: handled)", async () => {
    const late = sleep(25).then(() => { throw new Error("late"); });
    const got = await outcome(withTimeout(late, 5));
    await sleep(40); // give the late rejection its slot; withTimeout must have it handled
    // the timer's default message is unpinned (no `reason` passed); what the
    // doc pins is that the LATE rejection never replaces the outcome
    return got !== "late" && got !== "resolved" && got.length > 0;
  }, true],
]);

// ---------------------------------------------------------------------------
// retry (doc lines 8003-8013): 1 initial attempt + retries (default 3);
// backoffMs * factor^attempt (defaults 0 ms, 2); onRetry(error, attempt);
// the LAST error surfaces.
// ---------------------------------------------------------------------------
await runRowsAsync([
  ["sync fn returning a value works (doc: async or sync)", () => retry(() => "sync"), "sync"],
  ["first-try success: exactly one attempt", async () => { let calls = 0; const v = await retry(() => { calls++; return "v"; }); return calls + ":" + v; }, "1:v"],
  ["succeeds on attempt k after two failures", async () => {
    let calls = 0;
    const v = await retry(() => { calls++; if (calls < 3) throw new Error("f" + calls); return "ok"; }, { retries: 3, backoffMs: 0 });
    return calls + ":" + v;
  }, "3:ok"],
  ["retries exhausted -> the LAST error surfaces (1 + retries attempts)", async () => {
    let calls = 0; const errs = [];
    try { await retry(() => { calls++; const e = new Error("e" + calls); errs.push(e); throw e; }, { retries: 2, backoffMs: 0 }); return "no-throw"; }
    catch (e) { return calls + ":" + e.message + ":" + (e === errs[errs.length - 1]); }
  }, "3:e3:true"], // 1 + 2 retries = 3 attempts; the LAST error is e3 (doc)
  ["onRetry(error, attempt) observes each retry, attempts strictly increasing (numbering base not pinned)", async () => {
    const seen = [];
    await retry(() => { throw new Error("x"); }, { retries: 2, backoffMs: 0, onRetry: (err, attempt) => seen.push(attempt) }).catch(() => null);
    return seen.length === 2 && seen[1] > seen[0];
  }, true],
  ["default retries = 3 -> 4 total attempts before rejecting", async () => { let calls = 0; await retry(() => { calls++; throw new Error("x"); }).catch(() => null); return calls; }, 4],
  ["backoffMs/factor honored: still resolves after retries", async () => {
    let calls = 0;
    return retry(() => { calls++; if (calls < 2) throw new Error("x"); return "ok"; }, { retries: 3, backoffMs: 2, factor: 2 });
  }, "ok"],
]);

// ---------------------------------------------------------------------------
// Semaphore (doc lines 8015-8025): direct token hand-off (a release passes
// the token straight to the next waiter -- no newcomer can jump the queue).
// ---------------------------------------------------------------------------
await runRowsAsync([
  ["fresh Semaphore(n): available n, waiting 0", () => { const s = new Semaphore(2); return s.available === 2 && s.waiting === 0; }, true],
  ["acquire consumes a token; release restores it", async () => { const s = new Semaphore(2); const rel = await s.acquire(); const during = s.available; rel(); return during === 1 && s.available === 2; }, true],
  ["parked waiters are counted in `waiting`", async () => {
    const s = new Semaphore(1);
    const rel = await s.acquire();
    const w1 = s.acquire(); const w2 = s.acquire();
    await sleep(5);
    const waiting = s.waiting;
    rel();                      // direct hand-off wakes w1 with the token...
    const rel1 = await w1;
    rel1(); await w2;           // ...which w1 must pass on for w2 to run
    return waiting === 2;
  }, true],
  ["FIFO direct hand-off: the earliest waiter gets the released token, no jumping", async () => {
    const s = new Semaphore(1);
    const rel1 = await s.acquire();
    const order = [];
    const p2 = s.acquire().then((r) => { order.push("p2"); return r; });
    const p3 = s.acquire().then((r) => { order.push("p3"); return r; });
    await sleep(5);
    const parked = s.waiting === 2 && s.available === 0;
    rel1();
    const r2 = await p2;
    r2();                       // p2 passes the token on; a jump would serve p3 first
    await p3;
    return parked && JSON.stringify(order) === "[\"p2\",\"p3\"]";
  }, true],
  ["run(fn) resolves with fn's value and restores the token", () => { const s = new Semaphore(2); return s.run(async () => "v").then((v) => v + ":" + s.available); }, "v:2"],
  ["run(fn) propagates fn errors and restores the token", () => { const s = new Semaphore(2); return s.run(() => { throw new Error("job"); }).then(() => "resolved", (e) => e.message + ":" + s.available); }, "job:2"],
  ["run(fn) caps concurrent fns at n (interleave, not parallelism -- doc)", async () => {
    const s = new Semaphore(2);
    let cur = 0, max = 0;
    await Promise.all([1, 2, 3, 4].map(() => s.run(async () => { cur++; max = Math.max(max, cur); await sleep(6); cur--; })));
    return max;
  }, 2],
]);

// ---------------------------------------------------------------------------
// Queue (doc lines 8027-8039): synchronous bounded FIFO. push throws when
// closed or full; tryPush returns false; shift() is undefined when empty in
// ANY state. Refusal rows are SYNC throws (assertThrows would see a returned
// rejected promise as no-throw, proving synchronous refusal per the module
// comment).
// ---------------------------------------------------------------------------
runRows([
  ["FIFO push/shift order", () => { const q = new Queue(4); q.push(1); q.push(2); return [q.shift(), q.shift()]; }, [1, 2]],
  ["length getter tracks contents", () => { const q = new Queue(4); q.push(1); q.push(2); const len = q.length; q.shift(); return [len, q.length]; }, [2, 1]],
  ["push throws SYNCHRONOUSLY when full (doc: no silent drop)", () => { const q = new Queue(1); q.push(1); q.push(2); }, { throws: null }],
  ["tryPush false when full; true again after a shift", () => { const q = new Queue(1); q.push(1); const full = q.tryPush(2); q.shift(); return [full, q.tryPush(2)]; }, [false, true]],
  ["shift on empty OPEN queue -> undefined", () => new Queue(2).shift(), undefined],
  ["close() sets `closed`; push throws SYNCHRONOUSLY on closed; tryPush false", () => {
    const q = new Queue(2);
    q.close();
    const closed = q.closed;
    let pushThrew = false;
    try { q.push(1); } catch (e) { pushThrew = true; }
    return closed && pushThrew && q.tryPush(1) === false;
  }, true],
  ["shift on empty CLOSED queue -> undefined (doc: any state)", () => { const q = new Queue(2); q.close(); return q.shift(); }, undefined],
  ["already-queued values survive close() and still shift out", () => { const q = new Queue(2); q.push("v"); q.close(); return q.shift(); }, "v"],
]);

// ---------------------------------------------------------------------------
// Channel (doc lines 8041-8053): MPSC; send() backpressures at capacity;
// recv() resolves {value, done:false} then {done:true} after close AND drain;
// close() resolves a parked recv and REJECTS a send blocked on capacity.
// ---------------------------------------------------------------------------
await runRowsAsync([
  ["send under cap resolves immediately (buffered); length reflects it", async () => { const ch = new Channel(2); await ch.send(1); return ch.length; }, 1],
  ["send backpressures at capacity until a recv drains", async () => {
    const ch = new Channel(1);
    await ch.send(1);
    let settled = false;
    const parked = ch.send(2).then(() => { settled = true; });
    await sleep(5);
    const wasBlocked = settled === false;
    const r1 = await ch.recv();
    await sleep(5);
    await parked;
    return wasBlocked && r1.value === 1 && r1.done === false && settled === true;
  }, true],
  ["recv returns buffered values in FIFO send order", async () => { const ch = new Channel(2); await ch.send("a"); await ch.send("b"); const r1 = await ch.recv(); const r2 = await ch.recv(); return [r1.value, r2.value]; }, ["a", "b"]],
  ["close() resolves a PARKED recv with {done:true}", async () => {
    const ch = new Channel(1);
    let result = null;
    const p = ch.recv().then((r) => { result = r; });
    await sleep(5);
    ch.close();
    await sleep(5);
    await p;
    return result !== null && result.done === true && result.value === undefined;
  }, true],
  ["recv after close AND full drain -> {done:true}", async () => { const ch = new Channel(1); await ch.send(1); ch.close(); const a = await ch.recv(); const b = await ch.recv(); return a.done === false && a.value === 1 && b.done === true; }, true],
  ["close() REJECTS a send blocked on capacity (its value was never accepted); shape is an Error", async () => {
    const ch = new Channel(1);
    await ch.send(1);
    let err = null;
    const blocked = ch.send(2).catch((e) => { err = e; });
    await sleep(5);
    ch.close();
    await blocked;
    return err instanceof Error;
  }, true],
  ["async iterator drains buffered items then ends (Symbol.asyncIterator)", async () => {
    const ch = new Channel(2);
    await ch.send(1); await ch.send(2);
    ch.close();
    const got = [];
    for await (const v of ch) got.push(v);
    return got;
  }, [1, 2]],
  ["closed flag flips on close()", () => { const ch = new Channel(1); const before = ch.closed; ch.close(); return before === false && ch.closed === true; }, true],
]);

// ---------------------------------------------------------------------------
// pmap (doc lines 8056-8063): at most `concurrency` invocations in flight
// (default 8), results in ITEM order; the FIRST rejection rejects the whole
// pmap; in-flight items settle, late results dropped, late rejections handled.
// ---------------------------------------------------------------------------
await runRowsAsync([
  ["results in ITEM order regardless of completion order", () => pmap(["a", "b", "c"], async (x, i) => { await sleep(15 - i * 5); return x.toUpperCase(); }), ["A", "B", "C"]],
  ["concurrency cap: at most `concurrency` fns in flight (tracked via counter)", async () => {
    let cur = 0, max = 0;
    await pmap([1, 2, 3, 4, 5, 6, 7, 8], async () => { cur++; max = Math.max(max, cur); await sleep(5); cur--; }, { concurrency: 3 });
    return max;
  }, 3],
  ["default concurrency 8 (structural: >1 and never more than 8 in flight)", async () => {
    let cur = 0, max = 0;
    await pmap(new Array(12).fill(0), async () => { cur++; max = Math.max(max, cur); await sleep(5); cur--; });
    return max > 1 && max <= 8;
  }, true],
  ["sync fn allowed; identity mapping preserved in item order", () => pmap([1, 2, 3], (x) => x * 2), [2, 4, 6]],
  ["first rejection rejects the whole pmap; in-flight items still run to completion", async () => {
    let lateSettled = false;
    const got = await pmap([0, 1], async (x) => {
      if (x === 0) { await sleep(2); throw new Error("boom0"); }
      await sleep(15); lateSettled = true; return x;
    }).then(() => "resolved", (e) => e.message);
    await sleep(25);
    return got + ":" + lateSettled;
  }, "boom0:true"],
  ["late rejections after pmap settled are handled -- nothing surfaces twice (process survives)", async () => {
    const got = await pmap([0, 1], async (x) => {
      if (x === 0) { await sleep(2); throw new Error("first"); }
      await sleep(15); throw new Error("late");
    }).then(() => "resolved", (e) => e.message);
    await sleep(25);
    return got;
  }, "first"],
  ["empty items -> []", () => pmap([], async (x) => x), []],
]);

// ---------------------------------------------------------------------------
// Pool (doc lines 8065-8075): n submit slots; same honest interleave as pmap;
// a throwing job rejects its submit, frees its slot, and the pool keeps
// pumping.
// ---------------------------------------------------------------------------
await runRowsAsync([
  ["size getter", () => new Pool(3).size, 3],
  ["fresh pool: active 0, pending 0", () => { const p = new Pool(2); return p.active === 0 && p.pending === 0; }, true],
  ["submit resolves with the sync job value", () => new Pool(2).submit(() => 42), 42],
  ["submit resolves with the async job value", () => new Pool(2).submit(async () => "v"), "v"],
  ["active/pending reflect a full pool (cap 2, 3 jobs of 20ms)", async () => {
    const p = new Pool(2);
    const jobs = [p.submit(() => sleep(20).then(() => 1)), p.submit(() => sleep(20).then(() => 2)), p.submit(() => sleep(20).then(() => 3))];
    await sleep(2);
    const state = p.active === 2 && p.pending === 1;
    const vals = await Promise.all(jobs);
    return state && JSON.stringify(vals) === "[1,2,3]";
  }, true],
  ["throwing job rejects its submit; the pool keeps pumping (next submit works)", async () => {
    const p = new Pool(1);
    const threw = await p.submit(() => { throw new Error("job"); }).then(() => false, (e) => e.message === "job");
    const after = await p.submit(() => "after");
    return threw && after === "after";
  }, true],
  ["pool caps in-flight jobs at n", async () => {
    const p = new Pool(2);
    let cur = 0, max = 0;
    await Promise.all([1, 2, 3, 4].map(() => p.submit(async () => { cur++; max = Math.max(max, cur); await sleep(6); cur--; })));
    return max;
  }, 2],
]);

// ---------------------------------------------------------------------------
// debounce (doc lines 8077-8080): TRAILING -- the LAST call inside the window
// runs once, at the window's edge; cancel() drops a pending call.
// ---------------------------------------------------------------------------
await runRowsAsync([
  ["N rapid calls -> exactly ONE execution with the LAST args, at the window edge", async () => {
    const calls = [];
    const d = debounce((x) => calls.push(x), 15);
    d("a"); await sleep(2); d("b"); await sleep(2); d("c");
    const pendingMid = d.pending() === true;
    await sleep(30);
    return pendingMid && JSON.stringify(calls) === "[\"c\"]" && d.pending() === false;
  }, true],
  ["cancel() drops the pending call; pending() flips true -> false", async () => {
    const calls = [];
    const d = debounce((x) => calls.push(x), 15);
    d("x");
    const before = d.pending() === true;
    d.cancel();
    const after = d.pending() === false;
    await sleep(30);
    return before && after && calls.length === 0;
  }, true],
  ["a fresh window after the edge runs again", async () => {
    const calls = [];
    const d = debounce((x) => calls.push(x), 15);
    d("a"); await sleep(30);
    d("b"); await sleep(30);
    return JSON.stringify(calls) === "[\"a\",\"b\"]";
  }, true],
]);

// ---------------------------------------------------------------------------
// throttle (doc lines 8082-8085): LEADING call runs now, the last call in the
// window runs at the edge (trailing); cancel supported.
// ---------------------------------------------------------------------------
await runRowsAsync([
  ["leading call runs immediately (sync-visible effect)", () => {
    const calls = [];
    const t = throttle((x) => calls.push(x), 15);
    t("a");
    return JSON.stringify(calls) === "[\"a\"]";
  }, true],
  ["last call in the window runs at the edge (trailing); pending() flips", async () => {
    const calls = [];
    const t = throttle((x) => calls.push(x), 15);
    t("a"); t("b");
    const pendingMid = t.pending() === true;
    await sleep(30);
    return pendingMid && JSON.stringify(calls) === "[\"a\",\"b\"]";
  }, true],
  ["rapid burst -> leading + exactly one trailing with the LAST args", async () => {
    const calls = [];
    const t = throttle((x) => calls.push(x), 15);
    t("x"); t("y"); t("z");
    await sleep(30);
    return JSON.stringify(calls) === "[\"x\",\"z\"]";
  }, true],
  ["cancel() drops the trailing call", async () => {
    const calls = [];
    const t = throttle((x) => calls.push(x), 15);
    t("a"); t("b"); t.cancel();
    await sleep(30);
    return JSON.stringify(calls) === "[\"a\"]";
  }, true],
  ["after the window passes, the next call is leading again", async () => {
    const calls = [];
    const t = throttle((x) => calls.push(x), 15);
    t("a"); await sleep(30);
    const afterEdge = calls.length;
    t("b");
    return afterEdge === 1 && calls.length === 2;
  }, true],
]);

// ---------------------------------------------------------------------------
// Engine-internal prototype members (d.ts dyna:async, declared @internal per
// the AbortSignal._abort precedent): the runtime ships them on the public
// prototypes and the d.ts now documents them; these rows pin the surface so
// it cannot drift silently in either direction.
// ---------------------------------------------------------------------------
runRows([
  ["Semaphore._release exists as a function (d.ts @internal)", () => typeof Semaphore.prototype._release, "function"],
  ["Channel._admit exists as a function (d.ts @internal)", () => typeof Channel.prototype._admit, "function"],
  ["Pool._pump exists as a function (d.ts @internal)", () => typeof Pool.prototype._pump, "function"],
]);

print("bb_async: all tests passed (" + n + " assertions)");
