// covers: integration dyna:simd.spmv dyna:simd.f64Spmv dyna:ml.CSR -- the float32 and float64 forms share one CSR walk, checked against one dense reference and ml.CSR#toDense
// CSR sparse matrix-vector product y = A*x in dyna:ml's CSR layout (values, columns, rowPointers;
// duplicate columns in a row sum) against a dense reference built here: f64Spmv equals the
// left-to-right float64 row sum bit for bit; spmv (Float32Array) sums each row in float64 and
// rounds once. Also checked against ml.CSR#toDense of the same arrays, and every refusal.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_spmv");
import { spmv, f64Spmv } from "dyna:simd";
import { CSR } from "dyna:ml";
import { Random } from "dyna:random";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };
const throwsT = (f, cls, m) => { try { f(); ok(false, m + " did not throw"); } catch (e) { ok(e instanceof cls, m + " threw " + e.name + ": " + e.message); } };
const rng = new Random(3);

function randomCsr(rows, cols, density) {
    const vals = [], colsArr = [], rp = [0];
    for (let r = 0; r < rows; r++) {
        for (let c = 0; c < cols; c++)
            if (rng.nextFloat() < density) { vals.push(rng.normal()); colsArr.push(c); }
        if (cols && rng.nextFloat() < 0.2) { vals.push(rng.normal()); colsArr.push(rng.nextBounded(cols)); }
        rp.push(vals.length);
    }
    return { values: Float64Array.from(vals), columns: Int32Array.from(colsArr), rowPointers: Int32Array.from(rp) };
}

await __kitS.section("matches the dense reference", async () => {
    for (const [rows, cols, dens] of [[0, 0, 0], [1, 1, 1], [5, 3, 0.5], [40, 70, 0.1], [200, 30, 0.3], [3, 1000, 0.01]]) {
        const A = randomCsr(rows, cols, dens);
        const x64 = Float64Array.from({ length: cols }, () => rng.normal());
        const want = new Float64Array(rows);
        for (let r = 0; r < rows; r++) {
            let s = 0;
            for (let k = A.rowPointers[r]; k < A.rowPointers[r + 1]; k++) s += A.values[k] * x64[A.columns[k]];
            want[r] = s;
        }
        const y = new Float64Array(rows).fill(99);
        ok(f64Spmv(y, A.values, A.columns, A.rowPointers, x64) === y && y.every((v, i) => Object.is(v, want[i] + 0) || v === want[i]), `f64Spmv ${rows}x${cols}`);
        const v32 = Float32Array.from(A.values), x32 = Float32Array.from(x64), y32 = new Float32Array(rows);
        spmv(y32, v32, A.columns, A.rowPointers, x32);
        let bad = 0;
        for (let r = 0; r < rows; r++) {
            let s = 0;
            for (let k = A.rowPointers[r]; k < A.rowPointers[r + 1]; k++) s += v32[k] * x32[A.columns[k]];
            if (y32[r] !== Math.fround(s)) bad++;
        }
        ok(bad === 0, `spmv f32 ${rows}x${cols}: ${bad} rows differ`);
        if (rows && cols) {
            const m = new CSR(A.values, A.columns, A.rowPointers, cols);
            const D = await m.toDense();
            m.close();
            let worst = 0;
            for (let r = 0; r < rows; r++) {
                let s = 0, mag = 0;
                for (let c = 0; c < cols; c++) { s += D[r][c] * x64[c]; mag += Math.abs(D[r][c] * x64[c]); }
                worst = Math.max(worst, Math.abs(s - y[r]) / Math.max(mag, 1e-300));
            }
            ok(worst < 1e-13, `f64Spmv ${rows}x${cols} vs ml.CSR#toDense product: rel ${worst}`);
        }
    }
});

__kitS.section("refusals", () => {
    const v = Float64Array.of(1, 2, 3), c = Int32Array.of(0, 2, 1), rp = Int32Array.of(0, 2, 3), x = Float64Array.of(1, 1, 1);
    const y = Float64Array.of(7, 7);
    throwsT(() => f64Spmv(y, v, Int32Array.of(0, 3, 1), rp, x), RangeError, "column past x");
    throwsT(() => f64Spmv(y, v, Int32Array.of(0, -1, 1), rp, x), RangeError, "negative column");
    ok(y[0] === 7 && y[1] === 7, "bad column: y untouched");
    throwsT(() => f64Spmv(y, v, c, Int32Array.of(0, 3), x), RangeError, "rowPointers length != y.length + 1");
    throwsT(() => f64Spmv(y, v, c, Int32Array.of(1, 2, 3), x), RangeError, "rowPointers[0] != 0");
    throwsT(() => f64Spmv(y, v, c, Int32Array.of(0, 3, 2), x), RangeError, "decreasing rowPointers");
    throwsT(() => f64Spmv(y, v, c, Int32Array.of(0, 1, 2), x), RangeError, "rowPointers end != nnz");
    throwsT(() => f64Spmv(y, v, Int32Array.of(0, 1), rp, x), RangeError, "columns shorter than values");
    throwsT(() => f64Spmv(y, v, Uint32Array.of(0, 2, 1), rp, x), TypeError, "Uint32Array columns");
    throwsT(() => spmv(new Float32Array(2), Float32Array.of(1, 2, 3), c, rp, new Int32Array(3)), TypeError, "Int32Array x");
    throwsT(() => spmv(new Float32Array(2), v, c, rp, Float32Array.of(1, 1, 1)), TypeError, "Float64Array values to spmv");
    const buf = new Float64Array(6);
    throwsT(() => f64Spmv(buf.subarray(0, 2), v, c, rp, buf.subarray(1, 4)), RangeError, "y overlaps x");
});

__kitS.end();
print("test_simd_spmv: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
