// covers: c:src/object/property_set_convert.inc.c
// for-of and array destructuring over a plain Array (src/object/
// property_set_convert.inc.c js_for_of_start_mark / js_for_of_mark_next /
// js_for_of_close): when the array has no own Symbol.iterator, its prototype
// is this realm's Array.prototype whose Symbol.iterator is the original
// values function, and %ArrayIteratorPrototype%.next is the original native,
// the loop keeps the array and an index in its two iterator slots instead of
// allocating an ArrayIterator and looking up next. Nothing observable may
// change: length is read every step (growth and shrink during the loop),
// holes Get through the prototype, next is captured once when the loop
// starts (replacing %ArrayIteratorPrototype%.next or
// Array.prototype[Symbol.iterator] mid-loop does not affect the running
// loop, a replacement made before the loop does), and break/return/throw
// close the iterator: a "return" found anywhere on the ArrayIteratorPrototype
// chain (or a proxy in it) is called exactly once with a real Array Iterator
// positioned where the loop stopped. yield* keeps real iterators.
// Expected output pinned from the allocating build (m0, b9, c2cg).
// Proven by planting faults: ending at the dense count instead of the
// length (to slow row), reading a hole as undefined instead of Get (proto
// hole / sparse rows), skipping the "return" lookup on close (return rows),
// materializing at index 0 (return next= values), taking the marker for
// yield* (yield* rows), ignoring an own Symbol.iterator, and not checking
// %ArrayIteratorPrototype%.next at loop start (before-loop next rows); each
// turns this file red.
const out = [];
const log = (...a) => out.push(a.map((x) => typeof x === "string" ? x : JSON.stringify(x)).join(" "));
const collect = (a) => { const r = []; for (const x of a) r.push(x); return r; };

log("basic", collect([1, 2, 3]), collect([]), collect([1, , 3]));
Array.prototype[1] = "proto";
log("proto hole", collect([0, , 2]));
delete Array.prototype[1];
const grow = [1, 2];
let gi = 0;
for (const x of grow) { if (gi++ < 3) grow.push(x * 10); }
log("grow", grow);
const shrink = [1, 2, 3, 4, 5];
const sr = [];
for (const x of shrink) { sr.push(x); if (x === 2) shrink.length = 3; }
log("shrink", sr);
const toSlow = [1, 2, 3];
const ts = [];
for (const x of toSlow) { ts.push(x); if (x === 1) { toSlow[10] = 9; } }
log("to slow", ts.length, ts.slice(0, 4));
const g = [1, 2, 3];
Object.defineProperty(g, 1, { get() { return "getter"; } });
log("getter elem", collect(g));
const sparse = [];
sparse[3] = "x";
log("sparse", collect(sparse));

const [d1, , d3 = "def", ...drest] = [1, 2, undefined, 4, 5];
log("destructure", d1, d3, drest);
const [[n1, n2], [n3]] = [[1, 2], [3]];
log("nested destructure", n1, n2, n3);
let sw1 = 1, sw2 = 2;
[sw1, sw2] = [sw2, sw1];
log("swap", sw1, sw2);
function* gy(a) { yield* a; }
log("yield*", [...gy([1, 2, 3])], [...gy([])]);
function* gl(a) { for (const x of a) yield x * 2; }
log("gen for-of", [...gl([1, 2, 3])]);

const calls = [];
Object.defineProperty(Object.prototype, "return", {
    value() { calls.push(Object.prototype.toString.call(this) + " next=" + JSON.stringify(this.next())); return {}; },
    configurable: true, writable: true,
});
for (const x of [1, 2, 3]) { if (x === 2) break; }
try { for (const x of [4, 5, 6]) { if (x === 5) throw new Error("t"); } } catch (e) { calls.push("caught " + e.message); }
(function () { for (const x of [7, 8, 9]) return x; })();
const [z1] = [10, 11, 12];
for (const x of [1]) { }
const gen = gl([1, 2, 3]);
gen.next();
gen.return(0);
outer: for (const a of [1, 2]) { for (const b of [3, 4]) { continue outer; } }
log("return calls", calls);
delete Object.prototype.return;

const calls2 = [];
const IterProto = Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()));
Object.defineProperty(IterProto, "return", { get() { calls2.push("get " + Object.prototype.toString.call(this)); return undefined; }, configurable: true });
for (const x of [1, 2]) break;
log("return getter", calls2);
delete IterProto.return;
const ArrIterProto = Object.getPrototypeOf([][Symbol.iterator]());
const savedProto = Object.getPrototypeOf(IterProto);
const seenKeys = [];
Object.setPrototypeOf(IterProto, new Proxy(savedProto, { get(t, k, r) { seenKeys.push(String(k)); return Reflect.get(t, k, r); } }));
for (const x of [1, 2]) break;
Object.setPrototypeOf(IterProto, savedProto);
log("proxy in chain", seenKeys);

const origNext = ArrIterProto.next;
const origIter = Array.prototype[Symbol.iterator];
const mid = [];
for (const x of [1, 2, 3]) {
    mid.push(x);
    if (x === 1) { ArrIterProto.next = function () { return { done: true }; }; Array.prototype[Symbol.iterator] = function* () { yield "replaced"; }; }
}
log("mid-loop replace", mid);
const before = [];
for (const x of [1, 2, 3]) before.push(x);
log("before-loop replace iterator", before);
Array.prototype[Symbol.iterator] = origIter;
let nextCalls = 0;
ArrIterProto.next = function () { nextCalls++; return origNext.call(this); };
log("before-loop replace next", collect([5, 6]), nextCalls);
ArrIterProto.next = origNext;
log("restored", collect([7, 8]));
Object.defineProperty(ArrIterProto, "next", { get() { nextCalls += 100; return origNext; }, configurable: true });
log("next getter", collect([1]), nextCalls);
Object.defineProperty(ArrIterProto, "next", { value: origNext, writable: true, configurable: true });
const own = [1, 2];
own[Symbol.iterator] = function* () { yield "own"; };
log("own iterator", collect(own));
class Sub extends Array {}
log("subclass", collect(Sub.from([1, 2])));
log("non-arrays", collect("ab"), collect(new Set([1, 2])), collect(new Uint8Array([3, 4])), (function () { return collect(arguments); })(5, 6));
let total = 0;
for (let i = 0; i < 3000; i++) for (const x of [i, 1]) { if (x === 1) continue; total += x; }
log("loop", total);
function throwIn() {
    const seen = [];
    try { for (const a of [1, 2]) { for (const b of [3, 4]) { seen.push(a * b); if (b === 4) throw a; } } } catch (e) { seen.push("c" + e); } finally { seen.push("f"); }
    return seen;
}
log("nested throw", throwIn());

const want = [
    "basic [1,2,3] [] [1,null,3]",
    "proto hole [0,\"proto\",2]",
    "grow [1,2,10,20,100]",
    "shrink [1,2,3]",
    "to slow 11 [1,2,3,null]",
    "getter elem [1,\"getter\",3]",
    "sparse [null,null,null,\"x\"]",
    "destructure 1 def [4,5]",
    "nested destructure 1 2 3",
    "swap 2 1",
    "yield* [1,2,3] []",
    "gen for-of [2,4,6]",
    "return calls [\"[object Array Iterator] next={\\\"value\\\":3,\\\"done\\\":false}\",\"[object Array Iterator] next={\\\"value\\\":6,\\\"done\\\":false}\",\"caught t\",\"[object Array Iterator] next={\\\"value\\\":8,\\\"done\\\":false}\",\"[object Array Iterator] next={\\\"value\\\":11,\\\"done\\\":false}\",\"[object Array Iterator] next={\\\"value\\\":2,\\\"done\\\":false}\",\"[object Array Iterator] next={\\\"value\\\":4,\\\"done\\\":false}\",\"[object Array Iterator] next={\\\"value\\\":4,\\\"done\\\":false}\"]",
    "return getter [\"get [object Array Iterator]\"]",
    "proxy in chain [\"return\"]",
    "mid-loop replace [1,2,3]",
    "before-loop replace iterator [\"replaced\"]",
    "before-loop replace next [5,6] 3",
    "restored [7,8]",
    "next getter [1] 103",
    "own iterator [\"own\"]",
    "subclass [1,2]",
    "non-arrays [\"a\",\"b\"] [1,2] [3,4] [5,6]",
    "loop 4498499",
    "nested throw [3,4,\"c1\",\"f\"]",
];
const gotStr = out.join("\n"), wantStr = want.join("\n");
if (gotStr !== wantStr) {
    console.log("FAIL got:\n" + gotStr + "\nwant:\n" + wantStr);
    throw new Error("for-of array fast");
}
console.log("ok test_for_of_array_fast");
