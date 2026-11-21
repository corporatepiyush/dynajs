// covers: dyna:simd.f64Sum
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_f64_sum");
import { f64Sum, f64Dot, f64Max, f64Min, f64Scale, f64Axpy } from "dyna:simd";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }

const SIZES = [0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 100, 1000];
const rnd64 = (len, seed) => Float64Array.from({ length: len }, (_, i) => ((i * 7 + seed) % 17) - 8.5);

for (const len of SIZES) {
    const a = rnd64(len, 1), b = rnd64(len, 2);
    let s = 0, sCond = 0, d = 0, dCond = 0;
    for (let i = 0; i < len; i++) {
        s += a[i]; sCond += Math.abs(a[i]);
        d += a[i] * b[i]; dCond += Math.abs(a[i] * b[i]);
    }
    assert(Math.abs(f64Sum(a) - s) <= 1e-12 * Math.max(1, sCond), "f64Sum len=" + len);
    assert(Math.abs(f64Dot(a, b) - d) <= 1e-12 * Math.max(1, dCond), "f64Dot len=" + len);
}

assert(f64Sum(new Float64Array(0)) === 0, "f64Sum empty = 0");

__kitS.section("f64Dot length mismatch throws", () => {
    let t = false; try { f64Dot(new Float64Array(3), new Float64Array(4)); } catch { t = true; }
    assert(t, "f64Dot length mismatch throws");
    t = false; try { f64Axpy(new Float64Array(3), 1, new Float64Array(4)); } catch { t = true; }
    assert(t, "f64Axpy length mismatch throws");
    t = false; try { f64Sum(new Float32Array(4)); } catch { t = true; }
    assert(t, "f64Sum on a Float32Array throws");
    t = false; try { f64Sum(new Uint8Array(8)); } catch { t = true; }
    assert(t, "f64Sum on a Uint8Array throws");
    t = false; try { f64Sum([1, 2, 3]); } catch { t = true; }
    assert(t, "f64Sum on a plain array throws");
});
__kitS.end();

print("test_simd_f64: all tests passed (" + n + " assertions)");
