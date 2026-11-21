// covers: contract dyna:simd.cumsum dyna:simd.cummax -- one shared change in both scans: NEON prefix scans keep the running carry in a vector register
// The NEON Int32 cumsum and the Int32/Float32 cummax now scan 8 values per step (two 4-lane prefixes, the second
// lifted by the first's last lane, then both by the carry, which stays in a vector lane instead of moving to a general
// register and back); the Float32 cumsum keeps its exact per-4 arithmetic and only keeps the carry in a lane. Int32
// sums wrap like (a+b)|0 and max is exact, so the oracle is a plain JS scan, compared exactly, at every length around
// the 4/8 blocks, on unaligned views and with windows. Float32 cumsum is checked exactly on integers (every partial
// sum below 2^24, so any order is exact) and within the documented float32 drift otherwise. Float32 cummax with NaN
// keeps leading NaNs and carries the running maximum over later NaNs (the scalar NaN path); signed zeros compare
// equal (the vector max may return +0 where the tail keeps -0).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_cumsum_cummax_carry_oracle");
import { cumsum, cummax } from "dyna:simd";

let checks = 0, fails = 0;
let seed = 31337;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);
const same = (x, y) => x === y || (x !== x && y !== y);
function cmp(got, want, what, tol) {
    checks++;
    for (let i = 0; i < want.length; i++) {
        const ok = tol ? Math.abs(got[i] - want[i]) <= tol * Math.max(1, Math.abs(want[i])) : same(got[i], want[i]);
        if (!ok) {
            fails++;
            if (fails < 15) print("FAIL: " + what + " n=" + want.length + " at " + i + ": got " + got[i] + ", want " + want[i]);
            return;
        }
    }
}
const sizes = [];
for (let n = 0; n <= 40; n++) sizes.push(n);
sizes.push(63, 64, 65, 127, 1000, 4099, 65537);
const i32 = (n, mode) => Int32Array.from({ length: n }, () => (mode ? [0x7fffffff, -0x80000000, -1, 0, 1, 5][(rnd() * 6) | 0] : (rnd() * 4e9 - 2e9) | 0));
__kitS.section("Int32 cumsum and cummax equal a JS scan", () => {
    for (const n of sizes) for (const mode of [0, 1]) for (const off of [0, 1, 3]) {
        const base = i32(n + 4, mode), a = base.subarray(off, off + n);
        const ws = [], wm = [];
        let s = 0, m = -0x80000000;
        for (const v of a) { s = (s + v) | 0; ws.push(s); m = v > m ? v : m; wm.push(m); }
        cmp(cumsum(Int32Array.from(a)), ws, "i32 cumsum");
        cmp(cummax(Int32Array.from(a)), wm, "i32 cummax");
        const w = Int32Array.from(base); cumsum(w, off, n);
        cmp(w.subarray(off, off + n), ws, "i32 cumsum window");
        checks++;
        if (!base.subarray(0, off).every((v, i) => v === w[i]) || !base.subarray(off + n).every((v, i) => v === w[off + n + i])) { fails++; print("FAIL: window wrote outside n=" + n); }
        const w2 = Int32Array.from(base); cummax(w2, off, n);
        cmp(w2.subarray(off, off + n), wm, "i32 cummax window");
    }
});
__kitS.section("Float32 cumsum: exact on integers, float32 drift otherwise", () => {
    for (const n of sizes) for (const off of [0, 2]) {
        const a = Float32Array.from({ length: n + 3 }, () => ((rnd() * 200) | 0) - 100).subarray(off, off + n);
        const want = []; let s = 0;
        for (const v of a) { s += v; want.push(s); }
        cmp(cumsum(Float32Array.from(a)), want, "f32 cumsum int");
        const b = Float32Array.from({ length: n }, () => rnd() * 2 - 1);
        const wb = []; let t = 0;
        for (const v of b) { t += v; wb.push(t); }
        const g = cumsum(Float32Array.from(b));
        checks++;
        for (let i = 0; i < n; i++) if (Math.abs(g[i] - wb[i]) > 1e-3 * Math.max(1, Math.sqrt(i + 1))) { fails++; print("FAIL: f32 cumsum drift n=" + n + " at " + i + ": " + g[i] + " vs " + wb[i]); break; }
    }
});
__kitS.section("Float32 cummax: random, signed zeros, infinities, NaN path", () => {
    const sp = [-0, 0, Infinity, -Infinity, 1e-45, -1e-45, 3.4e38, -3.4e38, 1, -1];
    for (const n of sizes) for (const mode of [0, 1, 2]) for (const off of [0, 1]) {
        const a = Float32Array.from({ length: n + 2 }, () => {
            const r = rnd();
            return mode === 0 ? rnd() * 100 - 50 : mode === 1 ? sp[(r * sp.length) | 0] : r < 0.2 ? NaN : rnd() * 10 - 5;
        }).subarray(off, off + n);
        const want = [];
        let i = 0;
        while (i < n && a[i] !== a[i]) want.push(NaN), i++;
        let run = a[i];
        for (; i < n; i++) { if (a[i] > run) run = a[i]; want.push(run); }
        cmp(cummax(Float32Array.from(a)), want, "f32 cummax mode" + mode);
    }
});
if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_simd_cumsum_cummax_carry_oracle: " + checks + " checks passed");
__kitS.end();
