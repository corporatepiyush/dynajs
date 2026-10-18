// Iterator helper protocol details (ECMA-262 27.1.4 Iterator.prototype helpers) and the fast paths
// beneath them: the inline array-iterator step, owned-argument callback calls, and fast-array
// appends in toArray/Array.prototype.map. Expected values follow the spec text; node 26 agrees.
let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
const IterProto = Object.getPrototypeOf(Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]())));

{
    const log = [];
    IterProto.return = function () { log.push("return"); return {}; };
    try {
        eq([1, 2].values().flatMap((x) => [x, x * 10]).toArray(), [1, 10, 2, 20], "flatMap flattens arrays");
        eq(log, [], "flatMap does not call return() on an inner iterator that finished");
    } finally { delete IterProto.return; }
}
{
    const log = [];
    const inner = { i: 0, next() { log.push("inner.next"); return { value: this.i++, done: false }; }, return() { log.push("inner.return"); return {}; }, [Symbol.iterator]() { return this; } };
    const outer = Object.setPrototypeOf({ next() { log.push("outer.next"); return { value: 1, done: false }; }, return() { log.push("outer.return"); return {}; } }, Iterator.prototype);
    const h = outer.flatMap(() => inner);
    h.next();
    eq(h.return(), { done: true }, "flatMap return() result");
    eq(log, ["outer.next", "inner.next", "inner.return", "outer.return"], "flatMap return() closes the inner iterator once, then the outer");
    eq(h.next(), { done: true }, "flatMap after return() is done");
}
{
    let gets = 0;
    const mk = () => { let i = 0; return { get next() { gets++; return () => (i < 3 ? { value: i++, done: false } : { value: undefined, done: true }); }, [Symbol.iterator]() { return this; } }; };
    eq([1, 2].values().flatMap(mk).toArray(), [0, 1, 2, 0, 1, 2], "flatMap over custom inner iterators");
    ok(gets === 2, "flatMap reads each inner iterator's next once (read " + gets + ")");
}
{
    const log = [];
    const inner = { next() { throw new Error("boom"); }, return() { log.push("inner.return"); return {}; }, [Symbol.iterator]() { return this; } };
    const outer = Object.setPrototypeOf({ next() { return { value: 1, done: false }; }, return() { log.push("outer.return"); return {}; } }, Iterator.prototype);
    let err = null;
    try { outer.flatMap(() => inner).toArray(); } catch (e) { err = e; }
    ok(err && err.message === "boom", "an inner next() error propagates");
    eq(log, ["outer.return"], "an inner next() error closes the outer iterator only");
}
for (const kind of ["map", "filter", "take", "drop", "flatMap"]) {
    const log = [];
    const res = { get done() { log.push("get done"); return true; }, get value() { log.push("get value"); return 1; } };
    const outer = Object.setPrototypeOf({ next() { return { value: 1, done: false }; }, return() { log.push("outer.return"); return res; } }, Iterator.prototype);
    const h = kind === "take" || kind === "drop" ? outer[kind](1) : outer[kind]((x) => (kind === "flatMap" ? [x] : x));
    h.next();
    eq(h.return(), { done: true }, kind + " return() result");
    eq(log, ["outer.return"], kind + " return() does not read the result of the underlying return()");
}
for (const kind of ["map", "filter", "flatMap", "takeWhile", "scan"]) {
    let n = 0;
    const thrower = (x) => { if (n++ === 0) throw new Error("x"); return kind === "flatMap" ? [x] : x; };
    const h = kind === "scan" ? [1, 2, 3].values().scan(thrower, 0) : [1, 2, 3].values()[kind](thrower);
    if (kind === "scan") eq(h.next(), { value: 0, done: false }, "scan yields its seed first");
    let threw = false;
    try { h.next(); } catch (e) { threw = e.message === "x"; }
    ok(threw, kind + ": callback error propagates");
    eq(h.next(), { done: true }, kind + ": a helper that threw is completed");
}
{
    const r = Object.setPrototypeOf({ next() { return { value: 1, done: false }; }, return() { return 5; } }, Iterator.prototype).map((x) => x);
    r.next();
    let err = null;
    try { r.return(); } catch (e) { err = e; }
    ok(err instanceof TypeError, "return() whose result is not an object throws TypeError");
}

{
    const a = [1, 2, 3];
    const out = [];
    for (const x of a.values().map((x) => { if (x === 1) a.push(4); return x; })) out.push(x);
    eq(out, [1, 2, 3, 4], "array growth during iteration is observed");
    const b = [1, 2, 3, 4];
    eq(b.values().map((x) => { if (x === 1) b.length = 2; return x; }).toArray(), [1, 2], "array shrink during iteration is observed");
    eq([1, , 3].values().toArray(), [1, undefined, 3], "holes read as undefined");
    const c = [1, 2, 3];
    Object.defineProperty(c, 1, { get() { return 20; } });
    eq(c.values().toArray(), [1, 20, 3], "accessor elements go through the getter");
    eq([..."abc"].values().map((s) => s.toUpperCase()).toArray(), ["A", "B", "C"], "string elements");
    const ta = new Int16Array([5, 6]);
    eq(ta.values().toArray(), [5, 6], "typed array iterator");
    eq([1, 2].keys().toArray(), [0, 1], "keys() iterator");
    eq([7, 8].entries().toArray(), [[0, 7], [1, 8]], "entries() iterator");
}
{
    const AIP = Object.getPrototypeOf([][Symbol.iterator]());
    const orig = AIP.next;
    let calls = 0;
    AIP.next = function () { calls++; return orig.call(this); };
    try {
        eq([1, 2, 3].values().map((x) => x * 2).toArray(), [2, 4, 6], "replaced %ArrayIteratorPrototype%.next still yields");
        ok(calls === 4, "replaced %ArrayIteratorPrototype%.next is called for every step (" + calls + ")");
    } finally { AIP.next = orig; }
}
{
    const it = [1, 2, 3].values();
    const own = { next: () => ({ value: 42, done: false }) };
    const h = Iterator.prototype.take.call(Object.assign(it, own), 2);
    eq(h.toArray(), [42, 42], "an own next on an array iterator wins");
}
{
    const big = Array.from({ length: 70000 }, (_, i) => i);
    const t = big.values().toArray();
    ok(t.length === 70000 && t[69999] === 69999 && t[12345] === 12345, "toArray of 70000 elements");
    const m = big.map((x) => x + 1);
    ok(m.length === 70000 && m[69999] === 70000, "Array.prototype.map of 70000 elements");
    let err = null;
    try { Array.from.call(function () { return Object.freeze([]); }, [1]); } catch (e) { err = e; }
    ok(err instanceof TypeError, "Array.from into a frozen array throws");
    class Sub extends Array {}
    const s = Sub.from([1, 2]);
    ok(s instanceof Sub && s.length === 2, "Array.from with a subclass");
    const sealed = [1, 2, 3].map(function (x) { return x; });
    ok(Object.isExtensible(sealed) && sealed.length === 3, "map result is an ordinary array");
}
{
    let acc = [];
    eq([1, 2, 3].values().reduce((a, x, i) => { acc.push(i); return a + x; }), 6, "reduce without initial value");
    eq(acc, [1, 2], "reduce index starts at 1 without an initial value");
    eq([1, 2, 3].values().reduce((a, x, i) => a + x * i, 0), 8, "reduce with initial value passes the index");
    const seen = [];
    [5, 6].values().forEach((x, i) => seen.push([x, i]));
    eq(seen, [[5, 0], [6, 1]], "forEach passes value and index");
    eq([1, 2, 3].values().find((x, i) => i === 2), 3, "find passes the index");
    const o = { v: 1 };
    eq([o].values().find((x) => x === o), o, "find returns the same object");
    ok([1, 2].values().some((x) => x === 2) && ![1, 2].values().every((x) => x === 1), "some/every");
    eq([1, 2, 3, 4].values().filter((x, i) => i % 2 === 0).toArray(), [1, 3], "filter passes the index");
    function sloppy(x) { x = 99; arguments[1] = 77; return x; }
    eq([1, 2].values().map(sloppy).toArray(), [99, 99], "a callback that writes its parameters");
    const keep = [{ a: 1 }, { a: 2 }];
    keep.values().map((x) => { x = null; return 0; }).toArray();
    eq(keep, [{ a: 1 }, { a: 2 }], "parameter writes do not affect the source elements");
}

{
    const AIP = Object.getPrototypeOf([][Symbol.iterator]());
    const orig = AIP.next;
    let calls = 0;
    AIP.next = function () { calls++; return orig.call(this); };
    try {
        eq([1, 2].values().flatMap((x) => [x, -x]).toArray(), [1, -1, 2, -2], "flatMap with a replaced array iterator next");
        ok(calls === 9, "outer and inner array iterators call the replaced next: 3 + 2 * 3 (" + calls + ")");
    } finally { AIP.next = orig; }
    let getterThis = null, getterReads = 0;
    Object.defineProperty(AIP, "next", { get() { getterReads++; getterThis = this; return orig; }, configurable: true });
    try {
        eq([1].values().flatMap((x) => [x, x]).toArray(), [1, 1], "flatMap with an accessor next");
        ok(getterThis !== null && getterThis !== AIP && Object.getPrototypeOf(getterThis) === AIP, "the next getter receives the array iterator");
        ok(getterReads === 2, "the accessor next is read once for the outer and once for the inner iterator (" + getterReads + ")");
    } finally { Object.defineProperty(AIP, "next", { value: orig, writable: true, configurable: true, enumerable: false }); }
    const own = [9, 9];
    own[Symbol.iterator] = function* () { yield "own"; };
    eq([1].values().flatMap(() => own).toArray(), ["own"], "an own Symbol.iterator on the returned array is used");
    let retThis = null, rets = 0;
    IterProto.return = function () { rets++; retThis = this; return {}; };
    try {
        const h = [1].values().flatMap(() => [10, 20, 30]);
        eq(h.next(), { value: 10, done: false }, "first inner value");
        h.return();
        ok(rets >= 1 && retThis && Object.getPrototypeOf(retThis) === AIP, "return() closes the inner array iterator with the real iterator as this");
        const keep = [1, 2, 3];
        const h2 = [1].values().flatMap(() => keep);
        const got = [h2.next().value];
        keep.push(4);
        keep[1] = 22;
        for (let r = h2.next(); !r.done; r = h2.next()) got.push(r.value);
        eq(got, [1, 22, 3, 4], "inner array mutation is observed");
        eq([1].values().flatMap(() => [1, , 3]).toArray(), [1, undefined, 3], "holes in the inner array read as undefined");
        const sparse = [];
        sparse[5] = "x";
        eq([1].values().flatMap(() => sparse).toArray().length, 6, "a sparse inner array yields its full length");
    } finally { delete IterProto.return; }
}

print(`iterator spec: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failures");
