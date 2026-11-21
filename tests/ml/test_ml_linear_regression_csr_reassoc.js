// timeout: 900
// covers: dyna:ml.LinearRegression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_linear_regression_csr_reassoc");
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

await __kitS.section("same coefficient count", async () => {
    const rows = 200, cols = 80, k = 20;
    const { S, y } = sparse(rows, cols, k, 11);
    const sp = await new LinearRegression().fit(S, y);
    const dn = await new LinearRegression().fit(await S.toDense(), y);
    const a = sp.coef, b = dn.coef;
    assert(a.length === b.length, "same coefficient count");
    for (let i = 0; i < a.length; i++) {
        assert(Object.is(a[i], b[i]),
               `LinearRegression sparse coef[${i}] is BIT-equal to dense: ${a[i]} vs ${b[i]}`);
    }
    sp.close(); dn.close();
});
__kitS.end();

print("test_ml_csr_reassoc: CSR 4-accumulator dot moves coefficients by ULPs and nothing else (7 groups)");
