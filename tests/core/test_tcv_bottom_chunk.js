// covers: c:src/vm/tailcall.inc.c
// The tail-call interpreter's vstack keeps its bottom chunk pinned: frames of plain functions entered from C while the
// vstack is empty (plain calls made directly by module code, async functions and generators, whose frames live on the C stack,
// and callbacks of Array/Iterator methods and sort comparators called from them) start 16 bytes into the bottom chunk, so returning to the empty
// stack never pops the chunk and the next entry never grows it. Before, every such callback paid a chunk grow and a
// chunk pop (iterator forEach from an async body cost 1.3x the same call from a plain function).
// - "classic": callbacks from async, generator and nested classic frames give the same values and stack traces as
//   from a plain function, including throws, a frame bigger than a 128 KiB chunk and repeated entries;
//   the very first frame pushed is bigger than a chunk, so the bottom chunk is sized for it plus the pad (ASan: a
//   chunk sized without the pad overflows by 16 bytes);
// - "clamp": every entry onto the empty vstack (js_call_tc_entry from C, js_tc_enter_block from the C-stack interpreter) re-clamps the chunk end to the C
//   stack left, exactly as the old first-chunk grow did, so recursion entered from a deep C stack overflows at the same
//   depth as before (a stale clamp lets it run a whole chunk deep);
// - "gate": iterator forEach, a sort comparator and a direct plain call made from an async body cost at most 1.12x the
//   same work in a plain function.
// v10b-30: the gate is skipped on a sanitizer build (ASan instruments the two paths unequally, so the ratio measures ASan); the
//   child probe "teardown: precise (build)" under DYNA_TEARDOWN_TRACE=1 is how a script tells (tests/core/test_exit_teardown.js).
import { perfCheck } from "../lib/perf.js";
import { Exec, args } from "dyna:sys";
import { sections } from "../lib/kit.js";
let bigBody = "let s = 0;";
for (let i = 0; i < 17000; i++) bigBody += "let v" + i + " = " + i + ";";
const big = new Function("x", bigBody + "return v0 + v16999 + x;");
const firstBig = big(1);
let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
const S = sections("test_tcv_bottom_chunk");
S.section("classic: first frame bigger than a chunk", () => eq(firstBig, 17000, "classic: the first frame of the runtime is bigger than a chunk (the bottom chunk must hold it plus the 16-byte pad)"));

const arr = Array.from({ length: 300 }, (_, i) => i);
function work() {
    const r = [];
    r.push(arr.map((x) => x * 2).reduce((a, x) => a + x, 0));
    r.push(arr.values().map((x) => x + 1).filter((x) => x % 3).toArray().length);
    r.push(arr.slice().reverse().sort((a, b) => a - b)[299]);
    let s = 0;
    arr.values().forEach((x) => { s += x; });
    r.push(s);
    try { arr.forEach((x) => { if (x === 7) throw new RangeError("cb" + x); }); } catch (e) { r.push(e.message); }
    return r;
}
const want = work();

async function viaAsync() { const out = []; for (let i = 0; i < 3; i++) out.push(work()); await 0; out.push(work()); return out; }
function* viaGen() { for (let i = 0; i < 3; i++) yield work(); }
await S.section("classic: async, generator and module callbacks", async () => {
    const asyncOut = await viaAsync();
    for (const r of asyncOut) eq(r, want, "classic: async body callbacks");
    for (const r of viaGen()) eq(r, want, "classic: generator body callbacks");
    for (let i = 0; i < 3; i++) eq(work(), want, "classic: module body callbacks");
});

async function bigAsync() { const out = []; for (let i = 0; i < 3; i++) out.push([1, 2].map((x) => big(x))); return out; }
await S.section("classic: frame bigger than a chunk", async () => {
    for (const r of await bigAsync()) eq(r, [17000, 17001], "classic: frame bigger than a chunk from an async body");
    eq([1, 2].map(big), [17000, 17001], "classic: frame bigger than a chunk as the callback itself");
});

function thrower(x) { if (x > 2) throw new TypeError("t" + x); return x; }
async function traceAsync() { try { [1, 2, 3].forEach(thrower); } catch (e) { return [e.message, /thrower/.test(e.stack)]; } return null; }
await S.section("classic: throw from a callback", async () => { eq(await traceAsync(), ["t3", true], "classic: throw from a callback entered on the empty vstack");
});

function rec(d) { return rec(d + 1) + 1; }
let probeDepth = -1;
function probe() { let d = 0; function r2(k) { d = k; return r2(k + 1) + 1; } try { r2(0); } catch (e) { if (!(e instanceof RangeError)) throw e; } probeDepth = d; }
async function nest(k) { if (k === 0) { probe(); return; } return nest(k - 1); }
async function depthAt(k) { probeDepth = -1; let threw = false; try { nest(k).catch(() => {}); } catch (e) { threw = true; } return threw ? -1 : probeDepth; }
await S.section("clamp: async entry near the C stack limit", async () => {
    await 0;
    const shallow = await depthAt(0);
    ok(shallow > 1000, "clamp: recursion from a shallow C stack runs deep (" + shallow + ")");
    let lo = 0, hi = 1;
    while (await depthAt(hi) > 0 && hi < 1 << 20) { lo = hi; hi *= 2; }
    while (hi - lo > 1) { const mid = (lo + hi) >> 1; if (await depthAt(mid) > 0) lo = mid; else hi = mid; }
    const deep = await depthAt(lo);
    ok(deep > 0 && deep < 100, "clamp: recursion entered near the C stack limit gets only the C stack left (" + deep + " frames vs " + shallow + " shallow, nest " + lo + "; a stale clamp runs a whole chunk deep)");
    const again = await depthAt(0);
    ok(Math.abs(again - shallow) <= 32, "clamp: the shallow depth is unchanged after deep entries (" + again + " vs " + shallow + "; a few frames of slack for the resume path, a stale clamp is off by thousands)");
});
async function depthJ(k) { probeDepth = -1; try { JSON.parse("[".repeat(k) + "1" + "]".repeat(k), (key, v) => { if (v === 1) probe(); return v; }); } catch (e) { return -1; } return probeDepth; }
await S.section("clamp: JSON.parse reviver near the C stack limit", async () => {
    let loJ = 0, hiJ = 1;
    while (await depthJ(hiJ) > 0 && hiJ < 1 << 20) { loJ = hiJ; hiJ *= 2; }
    while (hiJ - loJ > 1) { const mid = (loJ + hiJ) >> 1; if (await depthJ(mid) > 0) loJ = mid; else hiJ = mid; }
    const deepJ = await depthJ(loJ);
    ok(deepJ > 0 && deepJ < 100, "clamp: a reviver entered from C deep inside JSON.parse (" + loJ + " levels) gets only the C stack left (" + deepJ + " frames; a stale clamp runs a whole chunk deep)");
});

const n = 4096, data = Array.from({ length: n }, (_, i) => i), noop = () => {}, inc = (x) => x + 1;
function plainBest(which) { let b = Infinity; for (let r = 0; r < 9; r++) { const t = performance.now(); for (let i = 0; i < 60; i++) { if (which === 1) data.slice().sort((a, b) => a - b); else if (which === 2) { let s = 0; for (let j = 0; j < n; j++) s = inc(s); data[0] = s - n; } else data.values().forEach(noop); } b = Math.min(b, performance.now() - t); } return b; }
async function asyncBest(which) { let b = Infinity; for (let r = 0; r < 9; r++) { const t = performance.now(); for (let i = 0; i < 60; i++) { if (which === 1) data.slice().sort((a, b) => a - b); else if (which === 2) { let s = 0; for (let j = 0; j < n; j++) s = inc(s); data[0] = s - n; } else data.values().forEach(noop); } b = Math.min(b, performance.now() - t); } return b; }
const sanBuild = String(Exec(args()[0], ["-e", "1"], { timeoutMs: 20000, encoding: "utf8", env: { DYNA_TEARDOWN_TRACE: "1" } }).stderr).includes("teardown: precise (build)");
await S.section("gate: async-body callbacks cost like plain-function ones", async () => {
    for (const [which, name] of [[0, "iterator forEach"], [1, "sort comparator"], [2, "direct plain call"]]) {
        const p = plainBest(which), a = await asyncBest(which);
        if (sanBuild) { print("SKIP perf: gate: " + name + " " + (a / p).toFixed(2) + "x -- sanitizer build"); continue; }
        perfCheck(ok, a / p < 1.12, "gate: " + name + " from an async body costs " + (a / p).toFixed(2) + "x the plain-function call (want < 1.12)");
    }
});
S.end();
print(fail ? "FAIL " + fail + " of " + (pass + fail) : "PASS " + pass);
