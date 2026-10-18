// Fast paths of Array.prototype shift/unshift/pop/concat/fill/join and the head offset that makes
// shift O(1): a fast array's storage may start past its allocation (u.array.head), so every
// operation after a shift must see the same elements an unshifted array would. Expected values
// follow ECMA-262 and agree with node 26.
const print = globalThis.print ?? console.log;
let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function throws(ctor, fn, m) { let e = null; try { fn(); } catch (x) { e = x; } ok(e instanceof ctor, m + " throws " + ctor.name + " (got " + e + ")"); }

{
    const q = [];
    let out = 0, sum = 0;
    for (let i = 0; i < 100000; i++) {
        q.push(i);
        if (i % 3 === 2) { sum += q.shift(); out++; }
    }
    while (q.length) { sum += q.shift(); out++; }
    ok(out === 100000 && sum === 4999950000, "a push/shift queue of 100000 drains in order (" + out + ", " + sum + ")");
}
{
    const a = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
    eq([a.shift(), a.shift(), a.shift()], [0, 1, 2], "shift returns from the front");
    eq(a, [3, 4, 5, 6, 7, 8, 9], "after three shifts");
    a.push(10, 11);
    eq(a, [3, 4, 5, 6, 7, 8, 9, 10, 11], "push after shift");
    a.unshift(1, 2);
    eq(a, [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11], "unshift reuses the room left by shift");
    a.splice(4, 2, "x");
    eq(a, [1, 2, 3, 4, "x", 7, 8, 9, 10, 11], "splice after shift/unshift");
    eq(a.slice(2, 5), [3, 4, "x"], "slice after shift");
    eq(a.indexOf("x"), 4, "indexOf after shift");
    ok(a.includes(11) && !a.includes(0), "includes after shift");
    eq([...a].reverse()[0], 11, "spread + reverse after shift");
    eq(a.map((x) => typeof x).filter((t) => t === "string").length, 1, "map/filter after shift");
    a.sort((x, y) => String(x).localeCompare(String(y)));
    eq(a[0], 1, "sort after shift");
    eq(a.length, 10, "length after mixed edits");
    eq(JSON.parse(JSON.stringify(a)).length, 10, "JSON round trip after shift");
    eq(Array.from(a).length, 10, "Array.from after shift");
    a.length = 2;
    eq(a, [1, 10], "truncate after shift");
}
{
    const a = [];
    for (let i = 0; i < 1000; i++) a.unshift(i);
    ok(a.length === 1000 && a[0] === 999 && a[999] === 0, "1000 unshifts build a reversed array");
    for (let i = 0; i < 500; i++) a.shift();
    ok(a.length === 500 && a[0] === 499, "then 500 shifts");
    for (let i = 0; i < 2000; i++) a.push(-i);
    ok(a.length === 2500 && a[2499] === -1999 && a[0] === 499, "then 2000 pushes grow past the front room");
    const b = [1];
    b.shift();
    eq(b, [], "shifting the last element empties the array");
    b.push(7);
    b.unshift(6);
    eq(b, [6, 7], "an emptied array is reusable");
    eq([].shift(), undefined, "shift of an empty array");
    eq([].pop(), undefined, "pop of an empty array");
    const c = [1, 2, 3];
    eq(c.unshift(), 3, "unshift with no arguments returns the length");
}
{
    const a = [1, 2, 3];
    Object.defineProperty(a, "length", { writable: false });
    throws(TypeError, () => a.shift(), "shift with a non-writable length");
    eq(Object.keys(a), ["0", "1"], "the elements moved before the length write failed");
    eq([a[0], a[1], a.length], [2, 3, 3], "and length stayed 3");
    const b = [1, 2, 3];
    Object.defineProperty(b, "length", { writable: false });
    throws(TypeError, () => b.pop(), "pop with a non-writable length");
    throws(TypeError, () => b.unshift(0), "unshift with a non-writable length");
    throws(TypeError, () => Object.freeze([1, 2]).shift(), "shift of a frozen array");
    throws(TypeError, () => Object.freeze([1, 2]).unshift(0), "unshift of a frozen array");
}
{
    const seen = [];
    Object.defineProperty(Array.prototype, 3, { set(v) { seen.push(v); }, get() { return "proto"; }, configurable: true });
    try {
        const a = [1, 2, 3];
        a.unshift(0);
        eq(seen, [3], "unshift moving into an index with a prototype setter calls the setter");
        const f = new Array(5);
        f.fill(9);
        ok(seen.length === 2 && seen[1] === 9, "fill into a hole with a prototype setter calls the setter");
        const c = [1, 2].concat([3, 4]);
        eq(c, [1, 2, 3, 4], "concat defines, it does not set");
    } finally { delete Array.prototype[3]; }
    eq(new Array(4).fill(1), [1, 1, 1, 1], "fill of holes after the setter is gone");
}
{
    const args = (function () { return arguments; })(1, 2, 3);
    eq(Array.prototype.shift.call(args), 1, "shift on arguments");
    eq(Array.from(args), [2, 3], "arguments after shift");
    const obj = { 0: "a", 1: "b", length: 2 };
    eq(Array.prototype.shift.call(obj), "a", "shift on an array-like");
    eq(obj, { 0: "b", length: 1 }, "array-like after shift");
    const sparse = [1, , 3];
    eq(sparse.shift(), 1, "shift of a holey array");
    ok(!(0 in sparse) && sparse[1] === 3 && sparse.length === 2, "the hole moved down");
    const big = [];
    big[100000] = 1;
    eq(big.shift(), undefined, "shift of a sparse array");
    eq(big.length, 100000, "sparse length after shift");
}
{
    eq([1, 2].concat([3], 4, [[5]]), [1, 2, 3, 4, [5]], "concat mixes arrays and values");
    eq([1, , 3].concat([4]), [1, , 3, 4], "concat keeps holes");
    ok(!(1 in [1, , 3].concat([4])), "the hole stays a hole");
    const o = { length: 2, 0: "x", 1: "y", [Symbol.isConcatSpreadable]: true };
    eq([0].concat(o), [0, "x", "y"], "concat spreads a spreadable array-like");
    const ns = [9];
    ns[Symbol.isConcatSpreadable] = false;
    eq([0].concat(ns).length, 2, "a non-spreadable array is one element");
    let leaked = null;
    class Sp extends Array { static get [Symbol.species]() { return function () { return (leaked = []); }; } }
    const s = Sp.from([1, 2]);
    const r = s.concat([3, 4]);
    ok(r === leaked && r.length === 4 && r[3] === 4, "concat into a species array keeps length in sync");
    let midLength = -1;
    const probe = { get [Symbol.isConcatSpreadable]() { midLength = leaked.length; return false; } };
    s.concat([3, 4], probe);
    eq(midLength, 4, "a later argument's isConcatSpreadable getter sees the species array's length");
    const big = Array.from({ length: 70000 }, (_, i) => i);
    const cc = big.concat(big);
    ok(cc.length === 140000 && cc[139999] === 69999 && cc[70000] === 0, "concat of two 70000 arrays");
    const objs = [{ a: 1 }];
    const co = objs.concat(objs);
    ok(co[0] === co[1] && co[0] === objs[0], "concat copies references");
}
{
    eq(new Array(3).fill(0), [0, 0, 0], "fill of holes");
    eq([1, 2, 3, 4].fill(9, 1, 3), [1, 9, 9, 4], "fill a range");
    eq(new Array(5).fill(7, 2), [, , 7, 7, 7], "fill from the middle of a holey array");
    ok(!(0 in new Array(5).fill(7, 2)), "fill leaves the leading holes");
    const n = new Array(100000).fill(1);
    ok(n.length === 100000 && n[99999] === 1 && n.every((x) => x === 1), "fill of 100000 holes");
    const fo = new Array(2).fill({});
    ok(fo[0] === fo[1], "fill uses the same object");
}
{
    eq([1, 2, 3].join(), "1,2,3", "join ints");
    eq([1, "b", null, undefined, 2.5, -7, true].join("-"), "1-b---2.5--7-true", "join mixed");
    eq([-2147483648, 2147483647, 0].join(), "-2147483648,2147483647,0", "join int extremes");
    eq([[1, 2], [3]].join(";"), "1,2;3", "join nested arrays");
    let calls = 0;
    const a = [1, { toString() { calls++; a.length = 1; return "o"; } }, 3];
    eq(a.join(), "1,o,", "join sees the array shrink inside toString");
    eq(calls, 1, "toString called once");
    const big = Array.from({ length: 50000 }, (_, i) => i);
    eq(big.join("").length, 238890, "join of 50000 ints");
}
{
    let order = [];
    const r = [3, 4].map(function (x, i, arr) { order.push([x, i, arr.length, this.t]); return x * 2; }, { t: "this" });
    eq(r, [6, 8], "map with thisArg");
    eq(order, [[3, 0, 2, "this"], [4, 1, 2, "this"]], "map passes value, index, array and thisArg");
    const keep = [{ a: 1 }];
    keep.forEach(function (x) { x = null; arguments[0] = 1; });
    eq(keep, [{ a: 1 }], "a callback writing its parameters does not touch the array");
    const fl = [1, 2, 3, 4].filter((x, i) => i % 2);
    eq(fl, [2, 4], "filter");
    ok([1, 2].some((x) => x === 2) && ![1, 2].every((x) => x === 2), "some/every");
}
{
    const holder = [];
    for (let i = 0; i < 50; i++) holder.push({ v: i });
    for (let i = 0; i < 20; i++) holder.shift();
    globalThis.gc?.();
    if (typeof std !== "undefined" && std.gc) std.gc();
    ok(holder[0].v === 20 && holder[29].v === 49, "objects behind a head survive collection");
}

{
    const a = Array.from({ length: 20 }, (_, i) => i);
    eq(a.splice(0, 3), [0, 1, 2], "splice from the front returns the removed elements");
    eq(a[0], 3, "after a front splice");
    eq(a.splice(1, 2, "x"), [4, 5], "a front-side splice with an insert");
    eq(a.slice(0, 4), [3, "x", 6, 7], "the prefix moved up");
    a.unshift("u");
    eq(a.slice(0, 3), ["u", 3, "x"], "unshift reuses the room a front splice left");
    eq(a.splice(-2, 2), [18, 19], "a back-side splice still moves the suffix");
    eq(a.length, 15, "length after the splices");
    eq(a.join(), "u,3,x,6,7,8,9,10,11,12,13,14,15,16,17", "contents after the splices");
    const objs = Array.from({ length: 10 }, (_, i) => ({ i }));
    objs.splice(1, 3);
    eq(objs.map((o) => o.i), [0, 4, 5, 6, 7, 8, 9], "front splice keeps object references");
}

{
    const a = [1, 2, 3, 1.0, 4, NaN, -0, 0, "x", {}];
    eq([a.indexOf(1), a.indexOf(1, 1), a.indexOf(4, 2), a.includes(NaN), a.indexOf(NaN), a.indexOf(0), a.indexOf(-0), a.includes(0, 7), a.indexOf(9)],
        [0, 3, 4, true, -1, 6, 6, true, -1], "indexOf/includes over mixed values");
    const b = Array.from({ length: 10007 }, (_, i) => i);
    eq([b.indexOf(10006), b.indexOf(10006, 3), b.includes(5003, 5003), b.indexOf(-1), b.indexOf(4, 5), b.indexOf(4095), b.indexOf(4096, 1), b.indexOf(4097, 4097)],
        [10006, 10006, true, -1, -1, 4095, 4096, 4097], "indexOf across chunk boundaries and fromIndex values");
    const c = Array.from({ length: 100 }, (_, i) => i);
    c[51] = 1.5;
    c[52] = 52.0;
    eq([c.indexOf(50), c.indexOf(52), c.includes(1.5), c.indexOf(53), c.indexOf(51)], [50, 52, true, 53, -1], "a float among ints");
}

print(`array fast paths: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failures");
