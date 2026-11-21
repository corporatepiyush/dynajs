// covers: c:src/vm/tailcall.inc.c
// Frame parity for the tail-call interpreter's VM-stack frames (CONFIG_TAILCALL_INTERP=y, src/vm/tailcall.inc.c). There, JS->JS calls of
// plain bytecode functions (call0-3, call, call_method, the fused
// call_method0/1 forms, new on a bytecode constructor, tail calls) and C->JS
// entries run in frames on a heap "vstack" instead of recursing on the C
// stack: the callee owns its arguments, function and this, and on return or
// unwind the frame is dropped and the caller resumed. Everything here must
// print exactly what the computed-goto interpreter prints (the expected text
// at the bottom came from that build and holds in both):
// - throws that cross several frames to a catch/finally in an outer one, with
//   every frame's line:column ("unwind"), through a native callback ("native")
//   and out of a getter/valueOf reached from a frame ("reentry");
// - new: the base-constructor result rule (object vs primitive return, a tail
//   call in a constructor), new.target, derived constructors and super(),
//   a throwing constructor ("ctor");
// - arguments objects with fewer/more arguments than parameters, mapped and
//   strict, rest/default parameters ("args"); closures over a returned
//   frame's parameters and locals ("closure");
// - proper tail calls: unbounded self/mutual/method tail recursion, and a
//   tail call whose callee needs a bigger frame (falls back to a real call,
//   which keeps the caller in the backtrace) ("tail");
// - deep recursion: a RangeError (never a crash), caught, then recursion
//   again; a frame bigger than one 128 KiB vstack chunk ("deep");
// - generators and async functions calling plain functions between
//   yield/await points ("gen").
// A planted fault in the frame push/pop, the unwind, the constructor rule,
// argument ownership or the tail-call reuse changes one of these lines.
const out = [];
function where(e) {
    const m = [...e.stack.matchAll(/test_tailcall_frames\.js:(\d+):(\d+)/g)];
    return m.map((x) => x[1] + ":" + x[2]).join(",");
}
function lvl3(x) { if (x === 7) throw new TypeError("deep " + x); return x + 1; }
function lvl2(x) { const r = lvl3(x); return r * 2; }
function lvl1(x) { let t = 0; try { t = lvl2(x); } finally { t += 1000; } return t; }
function unwind(n) {
    let s = 0, caught = "";
    for (let i = 0; i < n; i++) {
        try { s += lvl1(i % 9); } catch (e) { caught += e.message + "@" + where(e) + ";"; }
    }
    return s + " " + caught.slice(0, 120);
}
out.push("unwind " + unwind(30));
function cb(x) { if (x > 3) throw new RangeError("cb" + x); return x * 3; }
function native(n) {
    let r = "";
    for (let i = 0; i < n; i++) {
        try { r += [1, 2, 3, i].map(cb).join("") + "|"; } catch (e) { r += e.message + "@" + where(e) + "|"; }
    }
    return r + [5, 3, 9, 1].sort((a, b) => a - b).join();
}
out.push("native " + native(6));
function reentry() {
    const o = { get g() { return pick(this.v); }, v: 2, valueOf() { return twice(this.v); } };
    function pick(v) { if (v > 2) throw new Error("pick" + v); return v; }
    function twice(v) { return v * 2; }
    let s = o.g + (o * 1);
    o.v = 3;
    try { s += o.g; } catch (e) { s += "/" + e.message + "@" + where(e); }
    return s;
}
out.push("reentry " + reentry());
function P(x) { this.x = x; }
function Q() { this.a = 1; return { b: 2 }; }
function R() { this.a = 1; return 5; }
function helper() { return 7; }
function objHelper() { return { h: 1 }; }
function T() { this.a = 1; return helper(); }
function U() { this.a = 1; return objHelper(); }
function NT() { return new.target === NT; }
class A { constructor(v) { this.v = v; } m() { return this.v; } }
class B extends A { constructor(v) { super(v * 2); this.w = new.target === B; } }
class D extends A { constructor() { return { z: 9 }; } }
class E extends A { constructor() { } }
class F { constructor(i) { if (i === 3) throw new SyntaxError("F" + i); this.i = i; } }
function ctor() {
    const r = [];
    for (let i = 0; i < 3; i++) {
        r.push(new P(i).x, JSON.stringify(new Q()), JSON.stringify(new R()), JSON.stringify(new T()), JSON.stringify(new U()));
        const b = new B(i);
        r.push(b.v, b.w, b.m(), b instanceof A, new NT() instanceof NT, NT(), JSON.stringify(new D()));
    }
    try { new E(); } catch (e) { r.push(e.constructor.name + ":" + e.message); }
    try { for (let i = 0; i < 5; i++) r.push(new F(i).i); } catch (e) { r.push(e.message + "@" + where(e)); }
    try { A(); } catch (e) { r.push(e.constructor.name); }
    return r.join(",");
}
out.push("ctor " + ctor());
function argsLoose(a, b) { b = 9; return arguments.length + ":" + [...arguments].join("/") + ":" + a; }
function argsStrict(a, b) { "use strict"; b = 9; return arguments.length + ":" + [...arguments].join("/"); }
function rest(a, ...r) { return a + "+" + r.length + "+" + r.join(""); }
function dflt(a, b = a * 2, c = b + 1) { return [a, b, c].join("."); }
function args() {
    const r = [];
    for (let i = 0; i < 2; i++) {
        r.push(argsLoose(), argsLoose(1), argsLoose(1, 2), argsLoose(1, 2, 3, 4), argsStrict(5), argsStrict(5, 6, 7));
        r.push(rest(1), rest(1, 2, 3), dflt(1), dflt(1, 5), dflt(1, 5, 0));
    }
    return r.join(" ");
}
out.push("args " + args());
function mk(p) { let loc = p * 10; const inc = () => ++loc + p; const get = () => loc + ":" + p; return [inc, get]; }
function closure() {
    const fs = [];
    for (let i = 0; i < 4; i++) fs.push(mk(i));
    fs[1][0]();
    fs[1][0]();
    fs[3][0]();
    return fs.map((f) => f[1]()).join(",");
}
out.push("closure " + closure());
function count(n, acc) { if (n === 0) return acc; return count(n - 1, acc + 1); }
function even(n) { if (n === 0) return true; return odd(n - 1); }
function odd(n) { if (n === 0) return false; return even(n - 1); }
const tm = { k: 0, step(n) { if (n === 0) return this.k; this.k++; return this.step(n - 1); } };
function small(n) { return big(n); }
function big(n) {
    let a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8;
    if (n === 1) throw new Error("big" + (a + b + c + d + e + f + g + h));
    return n;
}
function TC(n) { this.n = n; return count(n, 0); }
function tail() {
    const r = [count(300000, 0), even(200001), tm.step(100000), new TC(1000).n];
    try { small(1); } catch (e) { r.push(e.message + "@" + where(e)); }
    return r.join(",");
}
out.push("tail " + tail());
let depth = 0;
function down() { depth++; down(); }
function sum(n) { return n === 0 ? 0 : n + sum(n - 1); }
function deep() {
    const r = [];
    for (let k = 0; k < 3; k++) {
        depth = 0;
        try { down(); } catch (e) { r.push(e.constructor.name + (depth > 100 ? ">100" : "<=100")); }
    }
    r.push(sum(500));
    let body = "let s = 0;";
    for (let i = 0; i < 9000; i++) body += "let v" + i + " = " + i + ";";
    body += "for (let i = 0; i < 9000; i += 1000) s += eval('v' + i); return s + x;";
    const huge = new Function("x", body);
    r.push(huge(1), [2, 3].map(huge).join("/"));
    return r.join(",");
}
out.push("deep " + deep());
function sq(x) { return x * x; }
function* g(n) { for (let i = 0; i < n; i++) { const v = yield sq(i); if (v) throw new Error("g" + v); } return sq(n); }
async function af(n) { let s = 0; for (let i = 0; i < n; i++) { s += sq(await i); } return s; }
function gen() {
    const it = g(5);
    const r = [];
    let x;
    while (!(x = it.next()).done) r.push(x.value);
    r.push(x.value);
    const it2 = g(5);
    it2.next();
    try { it2.next(7); } catch (e) { r.push(e.message + "@" + where(e)); }
    return r.join(",");
}
out.push("gen " + gen());
function check(asyncVal) {
    out.push("async " + asyncVal);
    const got = out.join("\n");
    const want = "unwind 27234 deep 7@33:52,34:34,35:45,39:24,43:28;deep 7@33:52,34:34,35:45,39:24,43:28;deep 7@33:52,34:34,35:45,39:24,43:28;\nnative 3690|3693|3696|3699|cb4@44:49,48:36,52:28|cb5@44:49,48:36,52:28|1,3,5,9\nreentry 6/pick3@55:50,54:27,59:17,62:30\nctor 0,{\"b\":2},{\"a\":1},{\"a\":1},{\"h\":1},0,true,0,true,true,false,{\"z\":9},1,{\"b\":2},{\"a\":1},{\"a\":1},{\"h\":1},2,true,2,true,true,false,{\"z\":9},2,{\"b\":2},{\"a\":1},{\"a\":1},{\"h\":1},4,true,4,true,true,false,{\"z\":9},ReferenceError:this is not initialized,0,1,2,F3@75:62,84:51,88:24,TypeError\nargs 0::undefined 1:1:1 2:1/9:1 4:1/9/3/4:1 1:5 3:5/6/7 1+0+ 1+2+23 1.2.3 1.5.6 1.5.0 0::undefined 1:1:1 2:1/9:1 4:1/9/3/4:1 1:5 3:5/6/7 1+0+ 1+2+23 1.2.3 1.5.6 1.5.0\nclosure 0:0,12:1,20:2,31:3\ntail 300000,false,100000,1000,big36@119:33,116:21,125:16,128:24\ndeep RangeError>100,RangeError>100,RangeError>100,125250,36001,36002/36003\ngen 0,1,4,9,16,25,g7@148:93,158:14,161:22\nasync 285";
    if (got !== want) {
        console.log("FAIL got:\n" + got + "\nwant:\n" + want);
        throw new Error("tail-call frame parity");
    }
    console.log("ok test_tailcall_frames");
}
af(10).then(check, (e) => { console.log("FAIL async " + e); throw e; });
