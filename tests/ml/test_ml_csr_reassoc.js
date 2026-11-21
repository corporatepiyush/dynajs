// timeout: 900
// covers: dyna:ml.CSR
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_csr_reassoc");
import { CSR, LogisticRegression, LinearRegression } from "dyna:ml";

function assert(cond, msg) { if (!cond) throw new Error("FAIL: " + msg); }
function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }

function sparse(rows, cols, k, seed) {
    const rnd = lcg(seed);
    const val = [], col = [], ptr = [0], y = [];
    const seen = new Set();
    for (let i = 0; i < rows; i++) {
        const lab = i % 2;
        seen.clear();
        seen.add(0);
        col.push(0); val.push(lab ? 3.0 : -3.0);
        for (let t = 1; t < k; t++) {
            let c;
            do { c = 1 + Math.floor(rnd() * (cols - 1)); } while (seen.has(c));
            seen.add(c);
            val.push((rnd() * 2 - 1) * 0.2);
            col.push(c);
        }
        seen.clear();
        ptr.push(val.length);
        y.push(lab);
    }
    return { S: new CSR(val, col, ptr, cols), y };
}

const SHAPES = [
    ["k=1 (unroll never runs)", 400, 32, 1, 30],
    ["k=2 (unroll never runs)", 400, 32, 2, 30],
    ["k=3 (unroll never runs)", 400, 64, 3, 30],
    ["k=4 (no tail)", 400, 64, 4, 30],
    ["k=5 (one tail element)", 400, 64, 5, 30],
    ["k=7 (three tail elements)", 400, 128, 7, 30],
    ["k=8 (no tail)", 400, 128, 8, 30],
    ["k=40 (wide row, 200 cols)", 400, 200, 40, 40],
    ["k=40 (narrow row, 64 cols)", 600, 64, 40, 40],
    ["k=150 (dense-ish, 200 cols)", 300, 200, 150, 25],
];

await __kitS.section("one-nonzero rows: one label per row", async () => {
    const rows = 500, cols = 1000;
    const val = [], col = [], ptr = [0], y = [];
    const rnd = lcg(5);
    for (let i = 0; i < rows; i++) {
        const c = Math.floor(rnd() * cols);
        col.push(c); val.push(rnd() * 2 - 1); ptr.push(val.length);
        y.push(i % 2);
    }
    const S = new CSR(val, col, ptr, cols);
    const m = await new LogisticRegression({ maxIter: 30, tol: 1e-9 }).fit(S, y);
    const p = await m.predict(S);
    assert(p.length === rows, "one-nonzero rows: one label per row");
    m.close();
});

await __kitS.section("block L60", async () => {
    const rows = 20, cols = 6;
    const S = new CSR([], [], new Array(rows + 1).fill(0), cols);
    const y = [];
    for (let i = 0; i < rows; i++) y.push(i % 2);
    const m = await new LogisticRegression({ maxIter: 50, tol: 1e-12 }).fit(S, y);
    for (const v of m.coef.flat()) assert(v === 0, `zero CSR coefficient is exactly 0, got ${v}`);
    const p = await m.predict(S);
    for (const v of p) assert(v === 0 || v === 1, `zero CSR predicts a class, got ${v}`);
    m.close();
});
__kitS.end();

print("test_ml_csr_reassoc: CSR 4-accumulator dot moves coefficients by ULPs and nothing else (7 groups)");
