// covers: dyna:mathx.NDArray
// The nine reserved NDArray dtypes. ORACLES: float16 against the engine's own Float16Array conversion (ECMA-262
// roundTiesToEven straight from the double) AND against a JS grid model; bfloat16 against the same grid model (the bf16 grid
// is the float32 grid cut to 7 mantissa bits; ties to even, overflow to Infinity), which the float16 leg cross-validates;
// integers against the documented truncate-and-saturate table; float32 against Math.fround. The sweep includes ties,
// subnormals, the overflow edge, signed zero and NaN, plus values whose float32 rounding would create a false tie (a
// double-rounding trap: f64 -> f32 -> f16 differs from f64 -> f16 there). CONTROL: views and copies keep the dtype and
// the backing typed array class.
import { NDArray } from "dyna:mathx";
import { suite } from "../lib/kit.js";

const t = suite("test_mathx_nd_array_dtypes");

let seed = 12345;
const rnd = () => { seed = (seed * 1103515245 + 12345) >>> 0; return seed / 4294967296; };

const gridRound = (v, mant, emin, maxFinite) => {
    if (Number.isNaN(v)) return NaN;
    if (v === 0 || !Number.isFinite(v)) return v;
    const a = Math.abs(v);
    let e = Math.floor(Math.log2(a));
    while (2 ** e > a) e--;
    while (2 ** (e + 1) <= a) e++;
    const ulp = 2 ** (Math.max(e, emin) - mant);
    const lo = Math.floor(a / ulp);
    const rem = a / ulp - lo;
    let q = rem > 0.5 || (rem === 0.5 && lo % 2 === 1) ? lo + 1 : lo;
    let r = q * ulp;
    const limit = maxFinite + ulp / 2;
    if (r > maxFinite || a >= limit) r = Infinity;
    return v < 0 ? -r : r;
};
const f16ref = (v) => gridRound(v, 10, -14, 65504);
const bf16ref = (v) => gridRound(v, 7, -126, (2 - 2 ** -7) * 2 ** 127);

const probes = () => {
    const out = [0, -0, NaN, Infinity, -Infinity, 1, -1, 0.1, 1 / 3, 65504, 65519.99, 65520, 65536, 1e-8, 5.96e-8, 2.98e-8,
        2 ** -24, 2 ** -25, 2 ** -25 * 1.0000001, 3 * 2 ** -25, 2 ** -14, 2 ** -14 - 2 ** -25, 1 + 2 ** -11, 1 + 3 * 2 ** -11,
        1 + 2 ** -11 + 2 ** -40, 1 + 2 ** -8, 1 + 3 * 2 ** -8, 1 + 2 ** -8 - 2 ** -30, 3.4e38, 3.39e38, 2 ** -126, 2 ** -133,
        2 ** -134, 2 ** -149, 1e-45, 1.0009765625 + 2 ** -24, 2049, 2051, 4097];
    for (let i = 0; i < 3000; i++) {
        const mag = 2 ** Math.floor(rnd() * 80 - 40);
        out.push((rnd() < 0.5 ? -1 : 1) * mag * (1 + rnd()));
        const k = Math.floor(rnd() * 2048);
        out.push((1 + k / 1024 + 1 / 2048) * 2 ** Math.floor(rnd() * 30 - 15));
        out.push((1 + k / 128 / 8 + 1 / 256) * 2 ** Math.floor(rnd() * 200 - 100));
    }
    return out;
};

t.test("float16 matches Float16Array and the grid model", ({ ok, eq }) => {
    const vals = probes();
    const a = NDArray.from(vals, "float16");
    eq(a.data.constructor, Float16Array);
    const eng = new Float16Array(vals);
    let bad = 0, badModel = 0;
    for (let i = 0; i < vals.length; i++) {
        const got = a.get(i);
        if (!Object.is(got, eng[i])) { if (bad++ < 5) ok(false, "f16 of " + vals[i] + ": got " + got + ", Float16Array " + eng[i]); }
        if (!Object.is(eng[i], f16ref(vals[i]))) { if (badModel++ < 5) ok(false, "oracle disagreement at " + vals[i]); }
    }
    eq(bad, 0, "mismatches against Float16Array over " + vals.length + " values");
    eq(badModel, 0, "the grid model reproduces Float16Array");
    const back = NDArray.from(eng, "float64");
    for (let i = 0; i < 50; i++) ok(Object.is(back.get(i), eng[i]), "float16 -> float64 is exact");
    const bits = new Uint16Array(a.data.buffer);
    eq(bits[2] & 0x7c00, 0x7c00, "NaN keeps an all-ones exponent");
    eq(bits[1], 0x8000, "-0 keeps its sign");
});

t.test("bfloat16 rounds to nearest even on the bf16 grid", ({ ok, eq }) => {
    const vals = probes();
    const a = NDArray.from(vals, "bfloat16");
    eq(a.data.constructor, Uint16Array, "bf16 is stored as raw Uint16Array bits");
    let bad = 0;
    for (let i = 0; i < vals.length; i++) {
        const got = a.get(i), want = bf16ref(vals[i]);
        if (!Object.is(got, want)) { if (bad++ < 5) ok(false, "bf16 of " + vals[i] + ": got " + got + ", want " + want); }
    }
    eq(bad, 0, "mismatches over " + vals.length + " values");
    const f32 = new Float32Array(1), u32 = new Uint32Array(f32.buffer);
    for (let i = 0; i < 200; i++) {
        f32[0] = a.get(i + 40);
        eq(u32[0] & 0xffff, 0, "a bf16 value is a float32 with the low 16 bits zero");
        eq(a.data[i + 40], u32[0] >>> 16, "the stored bits are the top half of the float32");
    }
    const raw = new NDArray(new Uint16Array([0x3f80, 0xc000, 0x7f80, 0x7fc0, 0x0001]), [5], { dtype: "bfloat16" });
    eq(raw.get(0), 1); eq(raw.get(1), -2); eq(raw.get(2), Infinity); ok(Number.isNaN(raw.get(3))); eq(raw.get(4), 2 ** -133);
});

t.test("float32 is Math.fround", ({ eq }) => {
    const vals = probes();
    const a = NDArray.from(vals, "float32");
    let bad = 0;
    for (let i = 0; i < vals.length; i++) if (!Object.is(a.get(i), Math.fround(vals[i]))) bad++;
    eq(bad, 0);
});

t.test("integers truncate toward zero and saturate; NaN is 0", ({ deep }) => {
    const v = [0, -0, 1.9, -1.9, 127.5, 128, -128.9, -129, 255.9, 256, 32767.5, 32768, -32769, 2147483647.9, 2147483648, -2147483649, 1e300, -1e300, NaN, Infinity, -Infinity];
    deep(NDArray.from(v, "int8").toArray(), [0, 0, 1, -1, 127, 127, -128, -128, 127, 127, 127, 127, -128, 127, 127, -128, 127, -128, 0, 127, -128]);
    deep(NDArray.from(v, "uint8").toArray(), [0, 0, 1, 0, 127, 128, 0, 0, 255, 255, 255, 255, 0, 255, 255, 0, 255, 0, 0, 255, 0]);
    deep(NDArray.from(v, "int16").toArray(), [0, 0, 1, -1, 127, 128, -128, -129, 255, 256, 32767, 32767, -32768, 32767, 32767, -32768, 32767, -32768, 0, 32767, -32768]);
    deep(NDArray.from(v, "int32").toArray(), [0, 0, 1, -1, 127, 128, -128, -129, 255, 256, 32767, 32768, -32769, 2147483647, 2147483647, -2147483648, 2147483647, -2147483648, 0, 2147483647, -2147483648]);
    deep(NDArray.from(v, "bool").toArray(), [false, false, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, false, true, true]);
});

t.test("every dtype: typed array class, get/set, astype round trips, views keep the dtype", ({ eq, deep, ok }) => {
    const classes = { float64: Float64Array, float32: Float32Array, float16: Float16Array, bfloat16: Uint16Array, int32: Int32Array, int16: Int16Array, int8: Int8Array, uint8: Uint8Array, bool: Uint8Array };
    const small = [[0, 1, 2], [3, 4, 5]];
    for (const [dt, C] of Object.entries(classes)) {
        const a = NDArray.from(small, dt);
        eq(a.dtype, dt); eq(a.data.constructor, C, dt + " backing class");
        const want = dt === "bool" ? small.map((r) => r.map((x) => x !== 0)) : small;
        deep(a.toArray(), want, dt + " values");
        a.set(1, 2, 1);
        eq(a.get(1, 2), dt === "bool" ? true : 1, dt + " set/get");
        const v = a.T.slice("::-1");
        eq(v.dtype, dt, dt + " view dtype"); eq(v.data, a.data, dt + " view shares data");
        eq(v.copy().dtype, dt); eq(v.toTypedArray().constructor, C);
        for (const dt2 of Object.keys(classes)) {
            const b = a.astype(dt2);
            eq(b.dtype, dt2); ok(b.isContiguous);
            if (dt !== "bool" && dt2 !== "bool") deep(b.astype("float64").toArray(), [[0, 1, 2], [3, 4, 1]], dt + " -> " + dt2 + " -> float64");
        }
        const w = new NDArray(new C(6), [2, 3], dt === "bool" || dt === "bfloat16" ? { dtype: dt } : undefined);
        w.assign(a); deep(w.astype("float64").toArray(), a.astype("float64").toArray(), dt + " assign");
        w.fill(2); eq(w.get(0, 0), dt === "bool" ? true : 2, dt + " fill");
    }
    const mixed = NDArray.zeros([3], "int8");
    mixed.assign(NDArray.from([1.7, 300, -300], "float64"));
    deep(mixed.toArray(), [1, 127, -128], "assign converts across dtypes");
    const h = NDArray.zeros([2], "float16"); h.assign(new Float32Array([0.1, 70000]));
    deep(h.toArray(), [new Float16Array([0.1])[0], Infinity]);
    eq(NDArray.from(new Int16Array([1, -2])).dtype, "int16", "from keeps a typed array's dtype");
    eq(NDArray.from(new Int16Array([1, -2]), "float32").dtype, "float32");
    eq(new NDArray(new Uint8Array(2), [2], { dtype: "bool" }).dtype, "bool");
});

await t.run();
