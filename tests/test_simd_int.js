import {
    i32Sum, i32Min, i32Max, i32Dot, i32Add, i32Mul, i32Scale, cumsum, cummax,
} from "dyna:simd";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }

const SIZES = [0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 32, 33, 100, 1000];

const geni = (len, seed) =>
    Int32Array.from({ length: len }, (_, i) => (((i * 2654435761 + seed * 40503) >>> 0) % 2000001) - 1000000);
const genf = (len, seed) =>
    Float32Array.from({ length: len }, (_, i) => ((i * 7 + seed) % 2001) - 1000);

for (const len of SIZES) {
    const a = geni(len, 1), b = geni(len, 2);
    let s = 0, d = 0, mn = 0x7fffffff, mx = -0x80000000;
    for (let i = 0; i < len; i++) {
        s += a[i]; d += a[i] * b[i];
        if (a[i] < mn) mn = a[i];
        if (a[i] > mx) mx = a[i];
    }
    assert(i32Sum(a) === s, "i32Sum len=" + len);
    assert(i32Dot(a, b) === d, "i32Dot len=" + len);
    if (len === 0) {
        for (const [name, fn] of [["i32Min", i32Min], ["i32Max", i32Max]]) {
            let t = false; try { fn(a); } catch { t = true; }
            assert(t, name + " empty throws");
        }
    } else {
        assert(i32Min(a) === mn, "i32Min len=" + len);
        assert(i32Max(a) === mx, "i32Max len=" + len);
    }
}
assert(i32Sum(new Int32Array(0)) === 0, "i32Sum empty = 0");
assert(i32Dot(new Int32Array(0), new Int32Array(0)) === 0, "i32Dot empty = 0");

assert(i32Sum(Int32Array.of(2147483647, 2147483647, 2147483647)) === 6442450941, "i32Sum > INT32_MAX");
assert(i32Sum(Int32Array.of(-2147483648, -2147483648)) === -4294967296, "i32Sum negatives");

for (const len of SIZES) {
    const a = geni(len, 3), b = geni(len, 4);
    const so = new Int32Array(len), po = new Int32Array(len);
    const rs = i32Add(so, a, b), rp = i32Mul(po, a, b);
    assert(rs === so && rp === po, "i32Add/i32Mul return out, len=" + len);
    for (let i = 0; i < len; i++) {
        assert(so[i] === ((a[i] + b[i]) | 0), "i32Add[" + i + "] len=" + len);
        assert(po[i] === Math.imul(a[i], b[i]), "i32Mul[" + i + "] len=" + len);
    }
}

for (const len of SIZES) {
    for (const s of [7, -3, 65537, 0, -2147483648]) {
        const a = geni(len, 5), c = Int32Array.from(a);
        const r = i32Scale(c, s);
        assert(r === c, "i32Scale returns its array, len=" + len);
        for (let i = 0; i < len; i++) assert(c[i] === Math.imul(a[i], s), "i32Scale[" + i + "] len=" + len + " s=" + s);
    }
}

for (const len of SIZES) {
    const ai = geni(len, 6), ci = Int32Array.from(ai);
    const ri = cumsum(ci);
    assert(ri === ci, "cumsum returns its array (i32), len=" + len);
    let accI = 0;
    for (let i = 0; i < len; i++) { accI = (accI + ai[i]) | 0; assert(ci[i] === accI, "i32 cumsum[" + i + "] len=" + len); }

    const af = genf(len, 6), cf = Float32Array.from(af);
    const rf = cumsum(cf);
    assert(rf === cf, "cumsum returns its array (f32), len=" + len);
    let accF = 0;
    for (let i = 0; i < len; i++) { accF += af[i]; assert(cf[i] === accF, "f32 cumsum[" + i + "] len=" + len); }
}

for (const len of SIZES) {
    const ai = geni(len, 7), ci = Int32Array.from(ai);
    cummax(ci);
    let mI = -0x80000000;
    for (let i = 0; i < len; i++) { if (ai[i] > mI) mI = ai[i]; assert(ci[i] === mI, "i32 cummax[" + i + "] len=" + len); }

    const af = Float32Array.from({ length: len }, (_, i) => ((i * 13 + 5) % 977) / 7 - 70);
    const cf = Float32Array.from(af);
    cummax(cf);
    let mF = -Infinity;
    for (let i = 0; i < len; i++) { if (af[i] > mF) mF = af[i]; assert(cf[i] === mF, "f32 cummax[" + i + "] len=" + len); }
}

{
    const o = new Int32Array(1);
    i32Add(o, Int32Array.of(2147483647), Int32Array.of(1));
    assert(o[0] === -2147483648, "i32Add overflow wraps");
    i32Mul(o, Int32Array.of(2147483647), Int32Array.of(2));
    assert(o[0] === -2, "i32Mul low32");
    const cw = Int32Array.of(2147483647, 1, 2147483647, 2);
    cumsum(cw);
    assert(cw[0] === 2147483647 && cw[1] === -2147483648, "cumsum overflow wraps");
    const km = Int32Array.of(-10, -3, -8, 5, 2, 5);
    cummax(km);
    assert(km[0] === -10 && km[3] === 5 && km[5] === 5, "i32 cummax dip");
    const fm = Float32Array.of(-3, -5, -1, -2);
    cummax(fm);
    assert(fm[0] === -3 && fm[1] === -3 && fm[2] === -1 && fm[3] === -1, "f32 cummax negatives (identity -inf)");
}

function throws(fn) { try { fn(); return false; } catch { return true; } }
{
    assert(throws(() => i32Dot(new Int32Array(3), new Int32Array(4))), "i32Dot length mismatch throws");
    assert(throws(() => i32Add(new Int32Array(3), new Int32Array(3), new Int32Array(4))), "i32Add length mismatch throws");
    assert(throws(() => i32Sum(new Float32Array(4))), "i32Sum on Float32Array throws (strict type)");
    assert(throws(() => i32Sum(new Uint32Array(4))), "i32Sum on Uint32Array throws (strict type)");
    assert(throws(() => i32Sum(new Int16Array(4))), "i32Sum on Int16Array throws");
    assert(throws(() => i32Sum([1, 2, 3])), "i32Sum on a plain array throws");
    assert(throws(() => cumsum(new Float64Array(4))), "cumsum on Float64Array throws");
    assert(throws(() => cumsum(new Uint32Array(4))), "cumsum on Uint32Array throws");
    assert(throws(() => cummax([1, 2, 3])), "cummax on a plain array throws");
}

{
    const a = Int32Array.of(1, 2, 3, 4, 5);
    let calls = 0;
    i32Scale(a, { valueOf() { calls++; return 3; } });
    assert(calls === 1 && a[0] === 3 && a[4] === 15, "i32Scale coerces scalar first");
}

{
    const v = Float32Array.from([1, NaN, 2, 3, 4, 5, 6, 7]);
    cummax(v);
    const want = [1, 1, 2, 3, 4, 5, 6, 7];
    for (let i = 0; i < 8; i++)
        assert(v[i] === want[i] || (Number.isNaN(want[i]) && Number.isNaN(v[i])),
               "f32 cummax NaN carry [" + i + "]");
    const w = Int32Array.of(2147483647, 2147483647, 2147483647);
    cumsum(w);
    assert(w[1] === -2 && w[2] === 2147483645, "i32 cumsum multi-wrap exact");
}

print("test_simd_int: all tests passed (" + n + " assertions)");
