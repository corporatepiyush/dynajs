// covers: dyna:ml.LogisticRegression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_logistic_regression_review_esc_fit");
import { CSR, LinearRegression, LogisticRegression } from "dyna:ml";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };

const buf = new Float64Array(1), u32 = new Uint32Array(buf.buffer);
function key(a) {
    buf[0] = a;
    let hi = u32[1] >>> 0, lo = u32[0] >>> 0;
    if (hi & 0x80000000) { hi = (hi ^ 0xFFFFFFFF) >>> 0; lo = (lo ^ 0xFFFFFFFF) >>> 0;
                           if (lo === 0xFFFFFFFF) { lo = 0; hi = (hi + 1) >>> 0; } }
    else { hi = (hi ^ 0x80000000) >>> 0; }
    return [hi, lo];
}
function ulpDiff(a, b) {
    if (Object.is(a, b)) return 0;
    if (isNaN(a) || isNaN(b) || !isFinite(a) || !isFinite(b)) return Infinity;
    const ka = key(a), kb = key(b);
    return Math.abs((ka[0] - kb[0]) * 4294967296 + (ka[1] - kb[1]));
}
function maxUlp(A, B) {
    let m = 0;
    const a = Array.from(A).flat(9), b = Array.from(B).flat(9);
    for (let i = 0; i < Math.min(a.length, b.length); i++) m = Math.max(m, ulpDiff(a[i], b[i]));
    return m;
}

function data(rows, cols, seed) {
    let s = seed >>> 0;
    const rnd = () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
    const X = [];
    for (let i = 0; i < rows; i++) {
        const r = [];
        for (let j = 0; j < cols; j++)
            r.push(rnd() < 0.5 ? 0 : (rnd() - 0.5) * Math.pow(10, Math.floor(rnd() * 24) - 12));
        X.push(r);
    }
    const y = X.map(r => r.reduce((a, v, j) => a + v * (j % 2 ? 1 : -1), 0.5));
    const yc = X.map((r, i) => (y[i] > 0 ? 1 : (i % 3 === 0 ? 2 : 0)));
    return { X, y, yc };
}

const mk = async (X) => await CSR.fromDense(X);
function mkUnsorted(X) {
    const val = [], col = [], ptr = [0];
    for (const r of X) {
        const nz = [];
        for (let c = 0; c < r.length; c++) if (r[c] !== 0) nz.push([c, r[c]]);
        nz.reverse();
        for (const [c, v] of nz) { val.push(v); col.push(c); }
        ptr.push(val.length);
    }
    return new CSR(val, col, ptr, X[0].length);
}
function mkDup(X) {
    const val = [], col = [], ptr = [0];
    for (const r of X) {
        for (let c = 0; c < r.length; c++) {
            if (r[c] === 0) continue;
            val.push(r[c] / 2, r[c] / 2); col.push(c, c);
        }
        ptr.push(val.length);
    }
    return new CSR(val, col, ptr, X[0].length);
}

const gridN = (cols) => {
    const rows = [];
    for (const base of [data(2, 3, 9).X[0], [1, 2, 3], [0, 0, 0], [-1e5, 1e-5, 7]]) {
        const r = [];
        for (let j = 0; j < cols; j++) r.push(base[j % base.length] * (j >= 3 ? 3 : 1));
        rows.push(r);
    }
    return rows;
};

await __kitS.section("E5 logreg sparse fit is close to dense fit (equivalence class)", async () => {
    let coefUlp = 0, predUlp = 0, exact = 0, trials = 0;
    for (const seed of [7, 8, 9]) {
        const { X, yc } = data(14, 5, seed);
        const md = await new LogisticRegression({ maxIter: 200 }).fit(X, yc);
        const S = await mk(X);
        const ms = await new LogisticRegression({ maxIter: 200 }).fit(S, yc);
        trials++;
        coefUlp = Math.max(coefUlp, maxUlp(ms.coef, md.coef));
        predUlp = Math.max(predUlp, maxUlp(await ms.predictProba(gridN(5)), await md.predictProba(gridN(5))));
        if (maxUlp(ms.coef, md.coef) === 0) exact++;
        S.close(); md.close(); ms.close();
    }
    print(`INFO  logreg sparse-fit vs dense-fit (sorted idx, 3 seeds, multiclass): max coef ulp=${coefUlp}, max predictProba ulp=${predUlp}, bit-equal trials=${exact}/${trials}`);
    ok(coefUlp < 1e6 && predUlp < 1e6, "E5 logreg sparse fit is close to dense fit (equivalence class)",
       `coefUlp=${coefUlp} predUlp=${predUlp}`);
    // Whether the two walks happen to land on identical bits is a property of
    // the compiler and target (fused multiply-add contraction), not of the
    // algorithm: clang on arm64/macOS gives 0 of 3, GCC on arm64/Linux 3 of 3.
    // The contract is the equivalence class asserted above.
    print(`INFO  E5b logreg sparse fit vs dense fit bit-equal in ${exact}/${trials} trials (platform-dependent, not asserted)`);
});
__kitS.end();
print("review_esc_fit: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_esc_fit: " + fail + " failures");
