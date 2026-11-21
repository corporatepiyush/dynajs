// covers: c:src/vm/tailcall.inc.c
// In-interpreter iteration (src/vm/tailcall.inc.c, js_tc_iter_*): under
// CONFIG_TAILCALL_INTERP a call of Array.prototype.{forEach,map,filter,some,
// every,find,findIndex,reduce} on a JS_CLASS_ARRAY receiver with a plain
// (non-generator, non-async) bytecode callback runs its loop as a frame on
// the interpreter's value stack and enters the callback as an inline frame,
// instead of calling it from C once per element. The native frame is still
// linked, so a backtrace still shows "at forEach (native)". Everything
// observable must match the native loop it replaces: length read once, a
// HasProperty then a Get for each index that is not a dense element (find and
// findIndex Get every index, holes included), callback args (value, index,
// array) and arguments.length, thisArg (strict and sloppy), reduce's
// undefined this and its "empty array" TypeError, species construction for
// map/filter, early exit for some/every/find/findIndex, mutation of the array
// during the loop, a throw from the callback or from a getter unwinding
// through the loop frame into the caller's catch and finally, and the
// backtrace. Generators, async functions, bound functions, natives, class
// constructors and non-Array receivers take the native path and behave as
// before. The interrupt poll per callback is proven in test_tailcall_poll.js.
// Expected output pinned from the native-loop build (b9/c2).
// Proven by planting faults: dropping the HasProperty for holes (calls
// the callback for holes), entering the callback with the array's this
// instead of thisArg, passing 2 args instead of 3, skipping map's
// DefineProperty (holes in the result), not linking the native frame (the
// "at forEach (native)" lines vanish), freeing the result before returning
// (ASan), not polling per callback (test_tailcall_poll forms hang), and
// reading length each step (the shrink/grow rows change); each turns a
// suite red. The fused obj.method(local) form once wrote its operands past
// the frame's stack_size (crash with a temporary receiver and a closure):
// the "fused temp recv" row.
const out = [];
const log = (...a) => out.push(a.map((x) => typeof x === "string" ? x : JSON.stringify(x)).join(" "));
function where(e) {
    return e.stack.split("\n").map((l) => l.trim()).filter((l) => l.startsWith("at ")).slice(0, 4)
        .map((l) => l.replace(/\(.*test_tc_array_iter\.js:(\d+):(\d+)\)/, "($1:$2)")).join(" | ");
}

const dense = [3, 1, 4, 1, 5, 9, 2, 6];
const holey = [1, , 3, , 5];
log("forEach", (() => { const r = []; dense.forEach((v, i, a) => r.push(v * 10 + i, a === dense)); return r; })());
log("forEach holes", (() => { const r = []; holey.forEach((v, i) => r.push(i)); return r; })());
log("map", dense.map((v, i) => v * i), Object.keys(holey.map((v) => v)), holey.map((v) => v).length);
log("filter", dense.filter((v) => v & 1), holey.filter(() => true));
log("some", dense.some((v) => v > 8), dense.some((v) => v > 9), [].some(() => true));
log("every", dense.every((v) => v > 0), dense.every((v) => v < 9), [].every(() => false));
log("find", dense.find((v) => v > 4), dense.find((v) => v > 9), holey.find((v) => v === undefined));
log("findIndex", dense.findIndex((v) => v > 4), dense.findIndex((v) => v > 9), holey.findIndex((v) => v === undefined));
log("reduce", dense.reduce((s, v) => s + v), dense.reduce((s, v) => s + v, 100), holey.reduce((s, v, i) => s + "," + i, "x"), [7].reduce((s) => s + 1));

const calls = [];
dense.some((v, i) => { calls.push(i); return v === 4; });
dense.every((v, i) => { calls.push(i); return v !== 1; });
dense.find((v, i) => { calls.push(i); return v === 5; });
dense.findIndex((v, i) => { calls.push(i); return v === 9; });
log("early exit", calls);

log("argc", [1, 2].map(function () { return arguments.length; }), [1, 2].reduce(function () { return arguments.length; }, 0));
log("args", [5].map(function (v, i, a) { return [v, i, a.length, arguments[3]]; }));
const thisArg = { tag: "T" };
log("thisArg", [1].map(function () { return this === thisArg; }, thisArg), [1].map(function () { return this === globalThis; }),
    [1].map(function () { "use strict"; return this; }), [1].map(function () { "use strict"; return typeof this; }, 5),
    [1].map(function () { return typeof this; }, 5), [1].map(() => this === undefined, thisArg));
log("reduce this", [1, 2].reduce(function (s) { "use strict"; return s + typeof this; }, ""));
try { [].reduce((a, b) => a); } catch (e) { log("empty", e.constructor.name, e.message); }
try { [, ,].reduce((a, b) => a); } catch (e) { log("empty holes", e.constructor.name, e.message); }
log("extra args", [1, 2].reduce((s, v) => s + v, 0, "ignored"), [1, 2].map((v) => v + 1, undefined, "ignored"));

const trace = [];
const traced = [0, , 2, , 4];
Object.setPrototypeOf(traced, new Proxy(Array.prototype, {
    has(t, k) { if (typeof k === "string") trace.push("has " + k); return Reflect.has(t, k); },
    get(t, k, r) { if (typeof k === "string" && /^\d+$/.test(k)) trace.push("get " + k); return Reflect.get(t, k, r); },
}));
for (const m of ["forEach", "map", "filter", "some", "every", "find", "findIndex"]) {
    trace.length = 0;
    traced[m]((v, i) => { trace.push("cb " + i); return m === "every"; });
    log("trace " + m, trace);
}
trace.length = 0;
traced.reduce((s, v, i) => { trace.push("cb " + i); return s; });
log("trace reduce", trace);
Array.prototype[1] = "proto";
log("proto hole", holey.map((v) => v), holey.filter((v) => typeof v === "string"));
delete Array.prototype[1];
const getterArr = [1, 2, 3];
Object.defineProperty(getterArr, 1, { get() { trace.push("getter"); return 20; } });
trace.length = 0;
log("getter elem", getterArr.map((v) => v + 1), trace);

const grow = [1, 2, 3];
log("grow", grow.map((v, i, a) => { if (i === 0) a.push(99); return v; }), grow.length);
const shrink = [1, 2, 3, 4, 5];
log("shrink", shrink.map((v, i, a) => { if (i === 1) a.length = 3; return v; }), Object.keys(shrink.map((v, i, a) => v)));
const del = [1, 2, 3, 4];
const seen = [];
del.forEach((v, i, a) => { seen.push(v); if (i === 0) delete a[2]; });
log("delete", seen);
const toSlow = [1, 2, 3, 4];
const seen2 = [];
toSlow.forEach((v, i, a) => { seen2.push(v); if (i === 0) { a[100] = 7; a.foo = 1; } });
log("to slow", seen2, toSlow.length);
const replace = [1, 2, 3];
log("replace", replace.map((v, i, a) => { if (i === 0) { a[1] = "x"; a[2] = "y"; } return v; }));
let acc = [];
[1, 2, 3].forEach((v) => [10, 20].forEach((w) => acc.push(v + w)));
log("nested", acc);
log("nested map", [[1, 2], [3]].map((r) => r.map((v) => v * 2).reduce((s, v) => s + v, 0)));

class MyArr extends Array {}
const ma = MyArr.from([1, 2, 3]);
log("species", ma.map((v) => v) instanceof MyArr, ma.filter((v) => v > 1) instanceof MyArr, ma.filter((v) => v > 1).length);
const spOrder = [];
const sp = [1, 2];
sp.constructor = { get [Symbol.species]() { spOrder.push("species"); return function (n) { spOrder.push("ctor " + n); return {}; }; } };
const spRes = sp.map((v) => { spOrder.push("cb " + v); return v * 2; });
log("species order", spOrder, spRes);
const frozen = [1, 2];
frozen.constructor = { [Symbol.species]: function () { return Object.freeze([]); } };
try { frozen.map((v) => v); } catch (e) { log("define fail", e.constructor.name, where(e)); }
const badSp = [1];
badSp.constructor = { get [Symbol.species]() { throw new SyntaxError("sp"); } };
try { badSp.map(() => 1); } catch (e) { log("species throw", e.constructor.name, e.message, where(e)); }

function throwsAt(n) {
    const a = [1, 2, 3, 4];
    a.forEach((v) => { if (v === n) throw new TypeError("at " + v); });
}
try { throwsAt(3); } catch (e) { log("throw", e.message, where(e)); }
function finallyCaller() {
    const steps = [];
    try {
        try { [1, 2, 3].map((v) => { steps.push(v); if (v === 2) throw "raw"; return v; }); } finally { steps.push("finally"); }
    } catch (e) { steps.push("caught " + e); }
    return steps;
}
log("finally", finallyCaller());
function catchInside() {
    return [1, 2, 3].map((v) => { try { if (v === 2) throw v; return v; } catch (e) { return -e; } });
}
log("catch inside", catchInside());
function throwInNested() {
    try { [1, 2].forEach((v) => [3, 4].reduce((s, w) => { if (w === 4 && v === 2) throw new RangeError("deep"); return s + w; }, 0)); } catch (e) { return e.message + " " + where(e); }
}
log("nested throw", throwInNested());
function getterThrow() {
    const a = [1, , 3];
    Object.defineProperty(a, 1, { get() { throw new EvalError("g"); } });
    try { a.every(() => true); } catch (e) { return e.constructor.name + " " + where(e); }
}
log("getter throw", getterThrow());
function protoGetterThrow() {
    const a = [1, , 3];
    Object.setPrototypeOf(a, Object.create(Array.prototype, { 1: { get() { throw new URIError("p"); } } }));
    try { a.filter(() => true); } catch (e) { return e.constructor.name + " " + where(e); }
}
log("proto getter throw", protoGetterThrow());
let retTotal = 0;
for (let i = 0; i < 2000; i++) {
    try { [1, 2, 3].some((v) => { if (v === 3 && i % 3 === 0) throw i; return false; }); retTotal++; } catch (e) { retTotal += 2; }
}
log("loop throw", retTotal);

function* gen(v) { yield v; }
log("generator", [1, 2].map(gen).map((g) => g.next().value));
const asyncRes = [1, 2].map(async (v) => v * 3);
log("async", asyncRes.map((p) => p instanceof Promise));
const bound = function (v) { return this.k + v; }.bind({ k: 10 });
log("bound", [1, 2].map(bound));
log("native cb", ["1", "2", "x"].map(Number), [1, 2].map(String), [[1], [2]].map(Array.prototype.join.call.bind(Array.prototype.join)));
try { [1].forEach(class K {}); } catch (e) { log("class cb", e.constructor.name, e.message); }
try { [1].forEach(); } catch (e) { log("no cb", e.constructor.name, e.message); }
try { [1].map({}); } catch (e) { log("obj cb", e.constructor.name, e.message); }
const arrayLike = { length: 3, 0: "a", 2: "c" };
let lenReads = 0;
const lenLike = { get length() { lenReads++; return 2; }, 0: 1, 1: 2 };
log("array-like", Array.prototype.map.call(arrayLike, (v) => v + "!"), Array.prototype.reduce.call(lenLike, (s, v) => s + v), lenReads);
log("string recv", Array.prototype.map.call("ab", (c, i) => c + i));
const ta = new Uint8Array([1, 2, 3]);
log("typed array", Array.from(ta.map((v) => v * 2)), ta.some((v) => v > 2), [].map.call(ta, (v) => v + 1));
const proxied = new Proxy([1, 2], {});
log("proxy recv", proxied.map((v) => v * 5));

function withArgsObj() { return Array.prototype.map.call(arguments, (v) => v * 2); }
log("arguments recv", withArgsObj(1, 2, 3));
function FusedCtor() { var r = 0; function cb(v) { r += v; } [1, 2].forEach(cb); this.r = r; }
log("fused temp recv", new FusedCtor().r, (function () { function cb(v) { return v * 2; } return [3, 4].map(cb); })());
const closures = [1, 2, 3].map((v) => () => v);
log("closures", closures.map((f) => f()));
let counter = 0;
const big = Array.from({ length: 5000 }, (_, i) => i);
for (let r = 0; r < 20; r++) counter += big.reduce((s, v) => s + (v & 3), 0) + big.filter((v) => v % 7 === 0).length;
log("bulk", counter);
function deep(n) { return n === 0 ? 0 : [n].map((v) => deep(v - 1) + 1)[0]; }
log("deep 80", deep(80));
let rec = 0;
function overflow() { rec++; [1].forEach(overflow); }
try { overflow(); } catch (e) { log("overflow", e.constructor.name); }

const want = [
    "forEach [30,true,11,true,42,true,13,true,54,true,95,true,26,true,67,true]",
    "forEach holes [0,2,4]",
    "map [0,1,8,3,20,45,12,42] [\"0\",\"2\",\"4\"] 5",
    "filter [3,1,1,5,9] [1,3,5]",
    "some true false false",
    "every true false true",
    "find 5  ",
    "findIndex 4 -1 1",
    "reduce 31 131 x,0,2,4 7",
    "early exit [0,1,2,0,1,0,1,2,3,4,0,1,2,3,4,5]",
    "argc [3,3] 4",
    "args [[5,0,1,null]]",
    "thisArg [true] [true] [null] [\"number\"] [\"object\"] [false]",
    "reduce this undefinedundefined",
    "empty TypeError empty array",
    "empty holes TypeError empty array",
    "extra args 3 [2,3]",
    "trace forEach [\"cb 0\",\"has 1\",\"cb 2\",\"has 3\",\"cb 4\"]",
    "trace map [\"cb 0\",\"has 1\",\"cb 2\",\"has 3\",\"cb 4\"]",
    "trace filter [\"cb 0\",\"has 1\",\"cb 2\",\"has 3\",\"cb 4\"]",
    "trace some [\"cb 0\",\"has 1\",\"cb 2\",\"has 3\",\"cb 4\"]",
    "trace every [\"cb 0\",\"has 1\",\"cb 2\",\"has 3\",\"cb 4\"]",
    "trace find [\"cb 0\",\"get 1\",\"cb 1\",\"cb 2\",\"get 3\",\"cb 3\",\"cb 4\"]",
    "trace findIndex [\"cb 0\",\"get 1\",\"cb 1\",\"cb 2\",\"get 3\",\"cb 3\",\"cb 4\"]",
    "trace reduce [\"has 1\",\"cb 2\",\"has 3\",\"cb 4\"]",
    "proto hole [1,\"proto\",3,null,5] [\"proto\"]",
    "getter elem [2,21,4] [\"getter\"]",
    "grow [1,2,3] 4",
    "shrink [1,2,3,null,null] [\"0\",\"1\",\"2\"]",
    "delete [1,2,4]",
    "to slow [1,2,3,4] 101",
    "replace [1,\"x\",\"y\"]",
    "nested [11,21,12,22,13,23]",
    "nested map [6,6]",
    "species true true 2",
    "species order [\"species\",\"ctor 2\",\"cb 1\",\"cb 2\"] {\"0\":2,\"1\":4}",
    "define fail TypeError at map (native) | at <eval> (119:17)",
    "species throw SyntaxError sp at get [Symbol.species] (121:69) | at map (native) | at <eval> (122:16)",
    "throw at 3 at <anonymous> (126:56) | at forEach (native) | at throwsAt (126:14) | at <eval> (128:15)",
    "finally [1,2,\"finally\",\"caught raw\"]",
    "catch inside [1,-2,3]",
    "nested throw deep at <anonymous> (142:103) | at reduce (native) | at <anonymous> (142:46) | at forEach (native)",
    "getter throw EvalError at get (147:62) | at every (native) | at getterThrow (148:18) | at <eval> (150:32)",
    "proto getter throw URIError at get (153:94) | at filter (native) | at protoGetterThrow (154:19) | at <eval> (156:43)",
    "loop throw 2667",
    "generator [1,2]",
    "async [true,true]",
    "bound [11,12]",
    "native cb [1,2,null] [\"1\",\"2\"] [\"1\",\"2\"]",
    "class cb TypeError class constructors must be invoked with 'new'",
    "no cb TypeError not a function",
    "obj cb TypeError not a function",
    "array-like [\"a!\",null,\"c!\"] 3 1",
    "string recv [\"a0\",\"b1\"]",
    "typed array [2,4,6] true [2,3,4]",
    "proxy recv [5,10]",
    "arguments recv [2,4,6]",
    "fused temp recv 3 [6,8]",
    "closures [1,2,3]",
    "bulk 164300",
    "deep 80 80",
    "overflow RangeError",
];
const gotStr = out.join("\n"), wantStr = want.join("\n");
if (gotStr !== wantStr) {
    console.log("FAIL got:\n" + gotStr + "\nwant:\n" + wantStr);
    throw new Error("tc array iter");
}
console.log("ok test_tc_array_iter");
