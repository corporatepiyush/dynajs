// RESAMPLE's bucket start is t0 + floor((t - t0) / interval) * interval with ONE
// rounding (a fused multiply-add), so the edges are the same on every target.
// Computed with two roundings it depended on whether the compiler contracted
// the expression: arm64 fused it, baseline x86-64 could not, and a bucket edge
// moved by one ulp between them. The expected edge here is the exactly rounded
// value of the real-number expression, computed with BigInt -- an oracle that
// shares nothing with the engine's arithmetic.
import { DataFrame } from "dyna:dataframe";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };

const f64 = new Float64Array(1), u64 = new BigUint64Array(f64.buffer);
function split(x) {
    f64[0] = x;
    const bits = u64[0];
    const neg = bits >> 63n;
    const exp = Number((bits >> 52n) & 0x7ffn);
    let m = bits & ((1n << 52n) - 1n);
    let e;
    if (exp === 0) e = -1074; else { m |= 1n << 52n; e = exp - 1075; }
    return { m: neg ? -m : m, e };
}
// exactly rounded a*b + c, through a decimal string (Number() rounds once)
function exactFma(a, b, c) {
    const A = split(a), B = split(b), C = split(c);
    const k = Math.min(A.e + B.e, C.e);
    const n = A.m * B.m * (1n << BigInt(A.e + B.e - k)) + C.m * (1n << BigInt(C.e - k));
    if (k >= 0) return Number(n * (1n << BigInt(k)));
    const neg = n < 0n, mag = neg ? -n : n;
    const scaled = (mag * 5n ** BigInt(-k)).toString().padStart(-k + 1, "0");
    const s = scaled.slice(0, scaled.length + k) + "." + scaled.slice(scaled.length + k);
    return Number((neg ? "-" : "") + s);
}

// find times whose bucket edge differs between one and two roundings
const interval = 0.1, cases = [];
let seed = 12345;
const rnd = () => (seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648;
for (let i = 0; cases.length < 64 && i < 200000; i++) {
    const t = 1000 + rnd() * 1000;
    const t0 = Math.floor(1000 / interval) * interval;
    const q = Math.floor((t - t0) / interval);
    if (exactFma(q, interval, t0) !== t0 + q * interval) cases.push(t);
}
ok(cases.length >= 8, "the generator found times where one and two roundings disagree (" + cases.length + ")");
cases.sort((a, b) => a - b);
const times = new Float64Array([1000, ...cases]);
const vals = new Float64Array(times.length).fill(1);
const df = new DataFrame({ t: times, v: vals });
const r = df.RESAMPLE("t", interval, "count");
const starts = Array.from(r.TO_COLUMNS().bucket);
const t0 = Math.floor(times[0] / interval) * interval;
const want = [];
for (const t of times) {
    const s = exactFma(Math.floor((t - t0) / interval), interval, t0);
    if (!want.length || want[want.length - 1] !== s) want.push(s);
}
ok(starts.length === want.length, "bucket count " + starts.length + " vs " + want.length);
let bad = 0, first = "";
for (let i = 0; i < Math.min(starts.length, want.length); i++)
    if (starts[i] !== want[i]) { if (!bad) first = i + ": " + starts[i] + " vs " + want[i]; bad++; }
ok(bad === 0, bad + " bucket edges are not the singly rounded value (first " + first + ")");

print("test_dataframe_resample_edges: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_dataframe_resample_edges failed");
