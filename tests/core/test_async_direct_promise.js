// covers: integration c:src/vm/interpreter.inc.c c:src/builtins/builtin-promise.c c:src/object/property_set_convert.inc.c -- async completion, reaction pairs, closure shape
// v10b-24. An async function settles its own promise directly instead of calling a pair of resolving functions it allocates per
// call; completion still runs the full resolve steps (self-resolution TypeError, one observable Get of "then", thenable job).
// Await handles use the bare shape since they never reach user code. Pending reactions are allocated as one fulfil/reject pair
// recycled through a bounded per-runtime free list. Arrows and methods are created from the native function shape with
// length/name filled in place; their property order, attributes and values are those of the generic path.
import { suite } from "../lib/kit.js";

const t = suite("async_direct_promise");
const tick = () => new Promise((r) => setTimeout(r, 0));

t.test("completion order matches the spec job order", async (a) => {
    const log = [];
    async function f1() { return 1; }
    async function f2() { throw new Error("x"); }
    async function f3() { return { then(r) { log.push("thenable"); r(7); } }; }
    async function f4() { return Promise.resolve(9); }
    async function f5() { return { get then() { log.push("getthen"); return undefined; } }; }
    f1().then((v) => log.push("a" + v));
    f2().catch((e) => log.push("b" + e.message));
    f3().then((v) => log.push("c" + v));
    f4().then((v) => log.push("d" + v));
    Promise.resolve().then(() => log.push("t1")).then(() => log.push("t2")).then(() => log.push("t3")).then(() => log.push("t4"));
    f5().then((v) => log.push("g" + typeof v));
    await tick();
    a.eq(log.join(), "getthen,a1,bx,thenable,t1,gobject,c7,t2,d9,t3,t4");
});

t.test("self resolution rejects with TypeError; thrown primitives reject as is", async (a) => {
    async function self() { await null; return pr; }
    const pr = self();
    let e1; try { await pr; } catch (e) { e1 = e; }
    a.eq(e1 instanceof TypeError, true);
    async function thr() { throw 42; }
    let e2; try { await thr(); } catch (e) { e2 = e; }
    a.eq(e2, 42);
    async function rej() { return Promise.reject(new RangeError("r")); }
    let e3; try { await rej(); } catch (e) { e3 = e; }
    a.eq(e3 instanceof RangeError, true);
    async function thenThrows() { return { get then() { throw new SyntaxError("g"); } }; }
    let e4; try { await thenThrows(); } catch (e) { e4 = e; }
    a.eq(e4 instanceof SyntaxError, true);
    const p = (async () => 5)();
    a.eq(p instanceof Promise, true);
    a.eq(Object.getPrototypeOf(p), Promise.prototype);
});

t.test("then is read exactly once per completion and called in a later job", async (a) => {
    let gets = 0, calls = 0, sync = true;
    const th = { get then() { gets++; return (r) => { calls++; a.eq(sync, false); r("v"); }; } };
    const p = (async () => th)();
    a.eq(gets, 1);
    a.eq(calls, 0);
    sync = false;
    a.eq(await p, "v");
    a.eq(gets, 1);
    a.eq(calls, 1);
});

t.test("pending reaction pairs fire in registration order and recycle", async (a) => {
    const log = [];
    let res, rej;
    const p = new Promise((r) => { res = r; });
    const q = new Promise((_, j) => { rej = j; });
    for (let i = 0; i < 5; i++) { p.then((v) => log.push("f" + i + v), () => log.push("bad")); q.then(() => log.push("bad"), (e) => log.push("r" + i + e)); }
    res(":");
    rej("!");
    await tick();
    a.eq(log.join(), "f0:,f1:,f2:,f3:,f4:,r0!,r1!,r2!,r3!,r4!");
    for (let i = 0; i < 2000; i++) { const x = new Promise(() => {}); x.then(() => {}); x.catch(() => {}); }
    let n = 0;
    for (let i = 0; i < 3000; i++) n += await new Promise((r) => setTimeout(() => r(1), 0)).then((v) => v);
    a.eq(n, 3000);
    let s = 0;
    const ps = [];
    for (let i = 0; i < 600; i++) { let rr; const pp = new Promise((r) => { rr = r; }); ps.push(pp.then((v) => { s += v; })); rr(i); }
    await Promise.all(ps);
    a.eq(s, 599 * 600 / 2);
});

t.test("arrow and method closures keep generic function properties", (a) => {
    const ar = (x, y) => x;
    a.eq(ar.length, 2);
    a.eq(ar.name, "ar");
    a.eq(Object.getOwnPropertyNames(ar).join(), "length,name");
    const d = Object.getOwnPropertyDescriptor(ar, "length");
    a.eq([d.writable, d.enumerable, d.configurable].join(), "false,false,true");
    const dn = Object.getOwnPropertyDescriptor(ar, "name");
    a.eq([dn.value, dn.writable, dn.enumerable, dn.configurable].join(), "ar,false,false,true");
    a.eq(((a1, b1 = 1, ...c1) => 0).length, 1);
    a.eq((() => 0).name, "");
    const o = { m(p, q, r) {}, async am(p) {}, get g() { return 1; } };
    a.eq(o.m.length, 3);
    a.eq(o.m.name, "m");
    a.eq("prototype" in o.m, false);
    a.eq(o.am.name, "am");
    a.eq(Object.getOwnPropertyDescriptor(o, "g").get.name, "get g");
    function fn(p) {}
    a.eq(Object.getOwnPropertyNames(fn).includes("prototype"), true);
    delete ar.length;
    a.eq(ar.length, 0);
    Object.defineProperty(ar, "name", { value: "z" });
    a.eq(ar.name, "z");
    const ar2 = (x) => x;
    a.eq(ar2.length, 1);
    a.eq(ar2.name, "ar2");
    a.eq(Object.getPrototypeOf(ar2), Function.prototype);
    const k = "comp";
    const o2 = { [k]: () => 0 };
    a.eq(o2.comp.name, "comp");
});

await t.run();
