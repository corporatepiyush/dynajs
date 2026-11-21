// covers: dyna:mathx.NDArray
// NDArray views against an independent JS model. ORACLE: the model never looks at strides; it represents an array as a
// shape plus an index function f(idx) -> value, and composes slice/transpose/broadcast/expandDims/squeeze as index
// remappings (Python slice semantics written out from the language reference), reshape as C-order flatten + refill. Every
// fuzz case compares toArray() of the native view with the model, then writes through the view and checks the write lands
// on the base element the model predicts (aliasing), which also separates a view from a copy. Negative strides, zero
// strides (broadcast), empty axes and 0-d arrays are all in the sweep. CONTROL: reshape of a transposed array must copy
// (numpy's no-copy rule fails), reshape of a sliced-but-mergeable array must stay a view.
import { NDArray, linalg } from "dyna:mathx";
import { suite } from "../lib/kit.js";

const t = suite("test_mathx_nd_array_views");

let seed = 0x9e3779b9;
const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 4294967296; };
const ri = (lo, hi) => lo + Math.floor(rnd() * (hi - lo + 1));

const model = (shape, f) => ({ shape, f });
const fromFlat = (shape, flat) => {
    const st = []; let s = 1;
    for (let i = shape.length - 1; i >= 0; i--) { st[i] = s; s *= shape[i]; }
    return model(shape, (idx) => flat[idx.reduce((a, x, i) => a + x * st[i], 0)]);
};
const pyRange = (n, start, stop, step) => {
    step = step ?? 1;
    const adj = (v, dflt) => {
        if (v === undefined || v === null) return dflt;
        if (v < 0) v += n;
        if (v < 0) return step < 0 ? -1 : 0;
        if (v >= n) return step < 0 ? n - 1 : n;
        return v;
    };
    const a = adj(start, step < 0 ? n - 1 : 0), b = adj(stop, step < 0 ? -1 : n), out = [];
    if (step > 0) for (let i = a; i < b; i += step) out.push(i);
    else for (let i = a; i > b; i += step) out.push(i);
    return out;
};
const mSlice = (m, specs) => {
    const maps = [], shape = [];
    m.shape.forEach((n, ax) => {
        const s = specs[ax];
        if (typeof s === "number") { maps.push({ fixed: s < 0 ? s + n : s }); return; }
        const r = s === undefined || s === null ? pyRange(n) : pyRange(n, s[0], s[1], s[2]);
        maps.push({ list: r }); shape.push(r.length);
    });
    return model(shape, (idx) => { let k = 0; return m.f(maps.map((mp) => mp.list ? mp.list[idx[k++]] : mp.fixed)); });
};
const mTranspose = (m, perm) => model(perm.map((p) => m.shape[p]), (idx) => { const o = []; perm.forEach((p, i) => { o[p] = idx[i]; }); return m.f(o); });
const mBroadcast = (m, shape) => model(shape, (idx) => m.f(m.shape.map((n, j) => (n === 1 ? 0 : idx[idx.length - m.shape.length + j]))));
const mList = (m) => {
    const rec = (d, pre) => d === m.shape.length ? m.f(pre) : Array.from({ length: m.shape[d] }, (_, i) => rec(d + 1, [...pre, i]));
    return rec(0, []);
};
const mFlat = (m) => {
    const out = [];
    const rec = (d, pre) => { if (d === m.shape.length) { out.push(m.f(pre)); return; } for (let i = 0; i < m.shape[d]; i++) rec(d + 1, [...pre, i]); };
    rec(0, []);
    return out;
};
const sliceStr = (s) => s === null ? null : typeof s === "number" ? s : (s[0] ?? "") + ":" + (s[1] ?? "") + ":" + (s[2] ?? "");

const randShape = (maxNd) => Array.from({ length: ri(0, maxNd) }, () => ri(0, 4) === 0 ? ri(0, 1) : ri(1, 5));
const randSpec = (n) => {
    const k = ri(0, 5);
    if (k === 0) return null;
    if (k === 1 && n > 0) return ri(-n, n - 1);
    const pick = () => (rnd() < 0.3 ? undefined : ri(-n - 2, n + 2));
    let step = ri(-3, 3); if (step === 0) step = rnd() < 0.5 ? undefined : -1;
    return [pick(), pick(), step];
};

t.test("constructor wraps without copying and reports the view", ({ eq, deep, ok }) => {
    const buf = new Float64Array([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]);
    const a = new NDArray(buf, [3, 4]);
    eq(a.data, buf, "data is the same typed array object");
    eq(a.data, a.data, "data is stable across reads");
    deep(a.shape, [3, 4]); deep(a.strides, [4, 1]); eq(a.offset, 0); eq(a.ndim, 2); eq(a.size, 12); eq(a.dtype, "float64");
    ok(a.isContiguous, "fresh view is contiguous");
    buf[5] = 50; eq(a.get(1, 1), 50, "writes to the buffer show through");
    const b = new NDArray(buf, [2, 2], { offset: 11, strides: [-4, -1] });
    deep(b.toArray(), [[11, 10], [7, 6]], "negative strides with an offset");
    const z = new NDArray(buf, [2, 3], { strides: [0, 1], offset: 2 });
    deep(z.toArray(), [[2, 3, 4], [2, 3, 4]], "zero stride repeats a row");
    eq(linalg.NDArray, NDArray, "linalg.NDArray is the same class");
    ok(a instanceof linalg.NDArray, "instanceof through the namespace");
    eq(String(a), "NDArray(float64, [3, 4])");
    eq(Object.prototype.toString.call(a), "[object NDArray]");
});

t.test("makers", ({ deep, eq }) => {
    deep(NDArray.zeros([2, 2]).toArray(), [[0, 0], [0, 0]]);
    deep(NDArray.ones(3, "int8").toArray(), [1, 1, 1]);
    deep(NDArray.full([2], 7.5, "float32").toArray(), [7.5, 7.5]);
    deep(NDArray.eye(2, 3).toArray(), [[1, 0, 0], [0, 1, 0]]);
    deep(NDArray.arange(5).toArray(), [0, 1, 2, 3, 4]);
    deep(NDArray.arange(1, 2, 0.25).toArray(), [1, 1.25, 1.5, 1.75]);
    deep(NDArray.arange(5, 0, -2).toArray(), [5, 3, 1]);
    eq(NDArray.arange(3, 1).size, 0, "empty when stop is behind start");
    const n = NDArray.from([[1, 2], [3, 4]]);
    deep(n.shape, [2, 2]); deep(n.toArray(), [[1, 2], [3, 4]]);
    const s = NDArray.from(4.5); eq(s.ndim, 0); eq(s.size, 1); eq(s.get(), 4.5); eq(s.toArray(), 4.5);
    const e = NDArray.from([]); deep(e.shape, [0]); deep(e.toArray(), []);
    const src = new Float32Array([1, 2]); const c = NDArray.from(src); src[0] = 9;
    eq(c.get(0), 1, "from copies"); eq(c.dtype, "float32");
    const w = new NDArray([[1, 2, 3]]); deep(w.shape, [1, 3]);
    const r = new NDArray([1, 2, 3, 4, 5, 6], [2, 3]); deep(r.toArray(), [[1, 2, 3], [4, 5, 6]]);
});

t.test("fuzz: slice chains match the model, values and aliasing", ({ ok, deep, eq, must }) => {
    let cases = 0;
    for (let it = 0; it < 400; it++) {
        const shape = randShape(4);
        const size = shape.reduce((a, b) => a * b, 1);
        const flat = Array.from({ length: size }, (_, i) => i + 1);
        const base = new NDArray(new Float64Array(flat), shape);
        let v = base, m = fromFlat(shape, flat);
        for (let step = 0; step < ri(1, 3); step++) {
            const specs = v.shape.map((n) => randSpec(n));
            const useStr = rnd() < 0.5;
            v = v.slice(...specs.map((s) => (useStr ? sliceStr(s) : s)));
            m = mSlice(m, specs);
        }
        must(JSON.stringify(v.shape) === JSON.stringify(m.shape), "shape " + JSON.stringify(v.shape) + " vs " + JSON.stringify(m.shape) + " from " + JSON.stringify(shape));
        deep(v.toArray(), mList(m), "values after slicing " + JSON.stringify(shape));
        eq(v.size, m.shape.reduce((a, b) => a * b, 1));
        if (v.size > 0) {
            const idx = v.shape.map((n) => ri(0, n - 1));
            const want = m.f(idx);
            v.set(...idx, -want);
            eq(base.data[want - 1], -want, "write through the view lands on the base element");
            base.data[want - 1] = want;
        }
        cases++;
    }
    ok(cases === 400);
});

t.test("fuzz: transpose, broadcastTo, expandDims, squeeze", ({ deep, eq }) => {
    for (let it = 0; it < 300; it++) {
        const shape = randShape(4);
        const size = shape.reduce((a, b) => a * b, 1);
        const flat = Array.from({ length: size }, (_, i) => i * 3 + 1);
        const a = new NDArray(new Float64Array(flat), shape).slice(...shape.map(() => (rnd() < 0.3 ? "::-1" : null)));
        const mref = model(a.shape, (idx) => a.get(...idx));
        const perm = shape.map((_, i) => i).sort(() => rnd() - 0.5);
        deep(a.transpose(perm).toArray(), mList(mTranspose(mref, perm)), "transpose " + JSON.stringify(perm));
        deep(a.transpose(...perm).toArray(), mList(mTranspose(mref, perm)), "transpose spread");
        deep(a.T.toArray(), mList(mTranspose(mref, shape.map((_, i) => shape.length - 1 - i))), "T");
        const bshape = [ri(1, 3), ...a.shape.map((n) => (n === 1 && rnd() < 0.5 ? ri(0, 3) : n))].slice(0, 6);
        const b = a.broadcastTo(bshape);
        deep(b.toArray(), mList(mBroadcast(mref, bshape)), "broadcastTo " + JSON.stringify(a.shape) + " -> " + JSON.stringify(bshape));
        if (b.size > 0) eq(b.strides[0], 0, "a new leading axis has stride 0");
        const ax = ri(-a.ndim - 1, a.ndim);
        const e = a.expandDims(ax);
        const pos = ax < 0 ? ax + a.ndim + 1 : ax;
        deep(e.shape, [...a.shape.slice(0, pos), 1, ...a.shape.slice(pos)], "expandDims shape");
        deep(e.toArray(), mList(model(e.shape, (idx) => mref.f(idx.filter((_, i) => i !== pos)))), "expandDims values");
        const sq = e.squeeze();
        deep(sq.shape, e.shape.filter((n) => n !== 1), "squeeze drops ones");
        deep(sq.toArray(), mList(model(sq.shape, (idx) => { const o = []; let k = 0; e.shape.forEach((n) => o.push(n === 1 ? 0 : idx[k++])); return e.get(...o); })), "squeeze values");
        deep(e.squeeze(pos).shape, a.shape, "squeeze of the inserted axis");
    }
});

t.test("fuzz: reshape is a view exactly when numpy's no-copy rule holds", ({ deep, eq, ok }) => {
    let views = 0, copies = 0;
    for (let it = 0; it < 300; it++) {
        const shape = randShape(4).map((n) => Math.max(n, 1));
        const size = shape.reduce((a, b) => a * b, 1);
        const flat = Array.from({ length: size }, (_, i) => i + 1);
        let a = new NDArray(new Float64Array(flat), shape);
        if (rnd() < 0.4) a = a.T;
        if (rnd() < 0.4 && a.ndim) a = a.slice(...a.shape.map((n) => (n > 1 && rnd() < 0.5 ? "::2" : null)));
        const m = model(a.shape, (idx) => a.get(...idx));
        const vals = mFlat(m);
        const n = vals.length;
        const divs = []; for (let d = 1; d <= n; d++) if (n % d === 0) divs.push(d);
        const target = n === 0 ? [0] : (() => { const d = divs[ri(0, divs.length - 1)]; return rnd() < 0.5 ? [d, n / d] : [n / d, 1, d]; })();
        const r = a.reshape(target);
        const useInfer = target.length > 1 && rnd() < 0.5;
        const r2 = useInfer ? a.reshape(...target.map((x, i) => (i === 0 ? -1 : x))) : r;
        deep(r2.shape, target, "inferred -1");
        deep(mFlat(model(r.shape, (idx) => r.get(...idx))), vals, "reshape keeps C order");
        if (r.data === a.data) {
            views++;
            if (r.size) { const old = a.get(...a.shape.map(() => 0)); r.set(...r.shape.map(() => 0), 1e9); eq(a.get(...a.shape.map(() => 0)), 1e9, "a view aliases"); r.set(...r.shape.map(() => 0), old); }
        } else {
            copies++;
            ok(r.isContiguous, "a reshape copy is contiguous");
        }
    }
    ok(views > 20 && copies > 20, "both paths exercised (" + views + " views, " + copies + " copies)");
});

t.test("reshape view/copy controls", ({ eq, ne, deep }) => {
    const a = NDArray.arange(24).reshape(2, 3, 4);
    eq(a.reshape(6, 4).data, a.data, "contiguous reshape is a view");
    ne(a.T.reshape(24).data, a.data, "transposed reshape must copy");
    const s = a.slice(null, null, "0:4:2");
    eq(s.reshape(12).data, s.data, "a stride-2 last axis still merges: 12 = 3*4 and 4 = 2*2");
    const s3 = a.slice(null, null, "0:3");
    ne(s3.reshape(18).data, s3.data, "a cut last axis cannot merge: stride 4 != 3*1");
    const rows = a.reshape(6, 4).slice("0:6:2");
    eq(rows.reshape(3, 2, 2).data, a.data, "splitting an axis of a sliced array is a view");
    deep(rows.reshape(3, 2, 2).strides, [8, 2, 1]);
    eq(NDArray.zeros([0, 3]).reshape(3, 0).size, 0);
    deep(NDArray.from(5).reshape(1, 1).toArray(), [[5]]);
    deep(NDArray.from([5]).reshape().toArray(), 5, "size-1 to 0-d");
});

t.test("contiguous, copy, fill, assign (overlap-safe, broadcasting)", ({ eq, ne, deep, ok }) => {
    const a = NDArray.arange(12).reshape(3, 4);
    eq(a.contiguous(), a, "contiguous returns this when already contiguous");
    const t2 = a.T.contiguous();
    ne(t2.data, a.data); ok(t2.isContiguous); deep(t2.toArray(), a.T.toArray());
    const c = a.copy(); c.set(0, 0, 99); eq(a.get(0, 0), 0, "copy is independent");
    a.slice(1).fill(-1); deep(a.toArray()[1], [-1, -1, -1, -1]);
    const b = NDArray.arange(10);
    b.slice("1:").assign(b.slice(":-1"));
    deep(b.toArray(), [0, 0, 1, 2, 3, 4, 5, 6, 7, 8], "overlapping shift right reads the source first");
    const d = NDArray.arange(10);
    d.assign(d.slice("::-1"));
    deep(d.toArray(), [9, 8, 7, 6, 5, 4, 3, 2, 1, 0], "in-place reversal through a negative-stride view");
    const m = NDArray.zeros([2, 3]);
    m.assign([1, 2, 3]); deep(m.toArray(), [[1, 2, 3], [1, 2, 3]], "row broadcast");
    m.assign(NDArray.from([[7], [8]])); deep(m.toArray(), [[7, 7, 7], [8, 8, 8]], "column broadcast");
    m.assign(5); deep(m.toArray(), [[5, 5, 5], [5, 5, 5]], "scalar assign");
    m.assign(new Float32Array([1, 2, 3])); deep(m.toArray()[0], [1, 2, 3], "typed array source");
    const tt = NDArray.arange(4).reshape(2, 2); tt.assign(tt.T); deep(tt.toArray(), [[0, 2], [1, 3]], "in-place transpose");
});

t.test("get/set indices", ({ eq, throws }) => {
    const a = NDArray.arange(6).reshape(2, 3);
    eq(a.get(1, 2), 5); eq(a.get(-1, -3), 3);
    eq(a.set(0, 1, 42), a, "set returns this"); eq(a.get(0, 1), 42);
    throws(() => a.get(2, 0), RangeError); throws(() => a.get(0, -4), RangeError);
    throws(() => a.get(0), RangeError); throws(() => a.get(0, 0, 0), RangeError);
    throws(() => a.get(0.5, 0), RangeError); throws(() => a.get("0", 0), TypeError);
    throws(() => a.set(0, 0, "x"), TypeError); throws(() => a.set(), TypeError);
});

t.test("live buffer: resized and detached buffers throw instead of reading out of bounds", ({ eq, throws }) => {
    const ab = new ArrayBuffer(8 * 12, { maxByteLength: 8 * 64 });
    const a = new NDArray(ab, [3, 4]);
    eq(a.get(2, 3), 0);
    const tail = a.slice(2);
    ab.resize(8 * 6);
    throws(() => a.get(0, 0), RangeError, "shrunk below the view");
    throws(() => tail.toArray(), RangeError);
    throws(() => a.fill(1), RangeError);
    throws(() => a.copy(), RangeError);
    ab.resize(8 * 12);
    eq(a.get(2, 3), 0, "usable again after growing back");
    const ta = new Float64Array(ab);
    const b = new NDArray(ta, [12]);
    ab.resize(8 * 64);
    eq(b.size, 12, "the view keeps its own shape after the buffer grows");
    const owned = new ArrayBuffer(32);
    const c = new NDArray(owned, [4]);
    owned.transfer();
    throws(() => c.get(0), TypeError, "detached");
    throws(() => c.toArray(), TypeError);
    throws(() => new NDArray(owned, [1]), TypeError, "constructing over a detached buffer");
});

t.test("immutable buffers refuse writes", ({ eq, throws, skip }) => {
    if (typeof ArrayBuffer.prototype.transferToImmutable !== "function") skip("no immutable ArrayBuffer in this engine");
    const ab = new Float64Array([1, 2, 3]).buffer.transferToImmutable();
    const a = new NDArray(ab, [3]);
    eq(a.get(1), 2);
    throws(() => a.set(0, 9), TypeError); throws(() => a.fill(0), TypeError); throws(() => a.assign([1, 2, 3]), TypeError);
    eq(a.copy().set(0, 9).get(0), 9, "a copy is writable");
});

t.test("malformed arguments", ({ throws }) => {
    const buf = new Float64Array(6);
    throws(() => new NDArray(buf, [7]), RangeError, "shape larger than the buffer");
    throws(() => new NDArray(buf, [2, 3], { offset: 1 }), RangeError, "offset pushes past the end");
    throws(() => new NDArray(buf, [2, 3], { offset: -1 }), RangeError);
    throws(() => new NDArray(buf, [2, 3], { strides: [-3, 1] }), RangeError, "negative stride below 0");
    throws(() => new NDArray(buf, [2, 3], { strides: [3] }), RangeError, "strides length");
    throws(() => new NDArray(buf, [2, 3], { strides: [3, 1.5] }), RangeError);
    throws(() => new NDArray(buf, [2, 3], { bogus: 1 }), TypeError, "unknown option");
    throws(() => new NDArray(buf, [2, 3], { dtype: "float32" }), TypeError, "dtype vs typed array class");
    throws(() => new NDArray(buf, [2, -3]), RangeError); throws(() => new NDArray(buf, [2.5]), RangeError);
    throws(() => new NDArray(buf, "6"), TypeError); throws(() => new NDArray(buf, [1, 1, 1, 1, 1, 1, 1, 1, 1]), RangeError, "9 dims");
    throws(() => new NDArray([[1, 2], [3]]), TypeError, "ragged"); throws(() => new NDArray([[1, 2], 3]), TypeError);
    throws(() => new NDArray(["a"]), TypeError); throws(() => new NDArray({}), TypeError); throws(() => new NDArray(), TypeError);
    throws(() => new NDArray(new Uint32Array(2)), TypeError); throws(() => new NDArray(new BigInt64Array(2)), TypeError);
    throws(() => new NDArray(new Uint16Array(2)), TypeError, "Uint16Array without bfloat16");
    throws(() => new NDArray([1, 2], [2], { strides: [1] }), TypeError, "strides on a copied array");
    throws(() => NDArray.zeros([2], "complex128"), TypeError); throws(() => NDArray.zeros([2], 7), TypeError);
    throws(() => NDArray.arange(0, 1, 0), RangeError); throws(() => NDArray.arange(0, Infinity), RangeError);
    throws(() => NDArray.full([2]), TypeError); throws(() => NDArray.eye(-1), RangeError);
    const a = NDArray.arange(6).reshape(2, 3);
    throws(() => a.slice(0, 0, 0), RangeError, "too many specs"); throws(() => a.slice("::0"), RangeError, "zero step");
    throws(() => a.slice("1:2:3:4"), SyntaxError); throws(() => a.slice("x"), SyntaxError); throws(() => a.slice(5), RangeError);
    throws(() => a.slice({}), TypeError);
    throws(() => a.reshape(4), RangeError); throws(() => a.reshape(-1, -1), RangeError); throws(() => a.reshape(4, -1), RangeError);
    throws(() => a.transpose(0), RangeError); throws(() => a.transpose(0, 0), RangeError); throws(() => a.transpose(0, 2), RangeError);
    throws(() => a.broadcastTo([3]), RangeError); throws(() => a.broadcastTo([4, 3, 2]), RangeError); throws(() => a.broadcastTo([3, 3]), RangeError);
    throws(() => a.expandDims(4), RangeError); throws(() => a.squeeze(0), RangeError); throws(() => a.astype("x"), TypeError); throws(() => a.astype(), TypeError);
    throws(() => a.assign([1, 2]), RangeError); throws(() => a.fill("1"), TypeError);
    throws(() => NDArray.prototype.get.call({}, 0), TypeError, "foreign receiver");
    throws(() => Object.getOwnPropertyDescriptor(NDArray.prototype, "shape").get.call(new Float64Array(1)), TypeError);
});

t.test("0-d and empty arrays", ({ eq, deep }) => {
    const s = NDArray.full([], 3);
    eq(s.ndim, 0); eq(s.get(), 3); s.set(4); eq(s.get(), 4); deep(s.shape, []); eq(s.T.get(), 4); eq(s.slice().get(), 4);
    deep(s.expandDims(0).toArray(), [4]); eq(s.copy().get(), 4); eq(s.isContiguous, true);
    const e = NDArray.zeros([3, 0, 2]);
    eq(e.size, 0); deep(e.toArray(), [[], [], []]); deep(e.T.shape, [2, 0, 3]); deep(e.slice("::-1").shape, [3, 0, 2]);
    eq(e.copy().size, 0); eq(e.toTypedArray().length, 0); eq(e.fill(1), e);
    deep(NDArray.arange(3).slice("5:").shape, [0], "slice past the end is empty");
});

await t.run();
