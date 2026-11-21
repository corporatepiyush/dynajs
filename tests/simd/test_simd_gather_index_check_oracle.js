// covers: contract dyna:simd.gather dyna:simd.f64Gather dyna:simd.scatterAdd dyna:simd.f64ScatterAdd -- one shared index check: the index check runs 64 indices per step without a branch
// Before the first write every index is checked against [0, n). The check now ORs an unsigned compare over blocks of
// 64 (a negative index is >= 2^31 as uint32; the bound is capped at 2^31 so arrays longer than that still reject
// exactly the negative ones), and a block that has a bad index is rescanned one by one so the error still names the
// first bad position. Oracle: gather/scatter results equal a JS loop bit for bit, and each bad index (-1, n, 2^31-1,
// -2^31) planted at the start, inside a 64-block, at a block edge and in the tail raises the documented RangeError
// naming that first position, with the outputs untouched.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_gather_index_check_oracle");
import { gather, f64Gather, scatterAdd, f64ScatterAdd } from "dyna:simd";

let checks = 0, fails = 0;
let seed = 2024;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);
function ok(c, what) { checks++; if (!c) { fails++; if (fails < 15) print("FAIL: " + what); } }
const sizes = [1, 2, 63, 64, 65, 127, 128, 129, 200, 1000, 4097];
__kitS.section("valid indices: results equal a JS loop", () => {
    for (const n of sizes) {
        const src = Float32Array.from({ length: n }, () => rnd() * 10 - 5), dsrc = Float64Array.from({ length: n }, () => rnd() * 10 - 5);
        const idx = Int32Array.from({ length: n }, () => (rnd() * n) | 0);
        const o = gather(new Float32Array(n), src, idx), od = f64Gather(new Float64Array(n), dsrc, idx);
        ok(o.every((v, i) => Object.is(v, src[idx[i]])), "gather n=" + n);
        ok(od.every((v, i) => Object.is(v, dsrc[idx[i]])), "f64Gather n=" + n);
        const want = new Float32Array(n), wd = new Float64Array(n);
        for (let i = 0; i < n; i++) { want[idx[i]] += src[i]; wd[idx[i]] += dsrc[i]; }
        const s = scatterAdd(new Float32Array(n), idx, src), sd = f64ScatterAdd(new Float64Array(n), idx, dsrc);
        ok(s.every((v, i) => Object.is(v, want[i])), "scatterAdd n=" + n);
        ok(sd.every((v, i) => Object.is(v, wd[i])), "f64ScatterAdd n=" + n);
        const edge = Int32Array.from({ length: n }, (_, i) => (i & 1 ? n - 1 : 0));
        ok(gather(new Float32Array(n), src, edge).every((v, i) => Object.is(v, src[edge[i]])), "gather 0/n-1 n=" + n);
    }
});
__kitS.section("bad indices: first bad position named, nothing written", () => {
    for (const n of sizes) {
        const src = Float32Array.from({ length: n }, (_, i) => i + 1), dsrc = Float64Array.from(src);
        const positions = [...new Set([0, 1, 30, 63, 64, 65, 100, n - 1].filter((p) => p < n))];
        for (const bad of [-1, n, 0x7fffffff, -0x80000000]) for (const p of positions) {
            const idx = Int32Array.from({ length: n }, () => (rnd() * n) | 0);
            idx[p] = bad;
            if (p + 3 < n) idx[p + 3] = -5;
            const msg = (fn) => fn + ": idx[" + p + "] = " + bad + " is outside [0, " + n + ")";
            for (const [fn, run, out] of [
                ["gather", (o) => gather(o, src, idx), new Float32Array(n).fill(7)],
                ["f64Gather", (o) => f64Gather(o, dsrc, idx), new Float64Array(n).fill(7)],
                ["scatterAdd", (o) => scatterAdd(o, idx, src), new Float32Array(n).fill(7)],
                ["f64ScatterAdd", (o) => f64ScatterAdd(o, idx, dsrc), new Float64Array(n).fill(7)],
            ]) {
                let m = "no error";
                try { run(out); } catch (e) { m = (e instanceof RangeError ? "" : "not RangeError: ") + e.message; }
                ok(m === msg(fn), fn + " n=" + n + " bad=" + bad + " at " + p + ": " + m);
                ok(out.every((v) => v === 7), fn + " wrote before the check n=" + n + " p=" + p);
            }
        }
    }
});
if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_simd_gather_index_check_oracle: " + checks + " checks passed");
__kitS.end();
