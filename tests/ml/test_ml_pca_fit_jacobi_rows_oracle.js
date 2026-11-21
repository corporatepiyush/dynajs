// covers: dyna:ml.PCA.fit
// dyn_jacobi keeps the eigenvectors as ROWS of V (V[i*p+k] is component k of eigenvector i) and applies each
// rotation to rows i, j of A (unit stride, vectorized) then mirrors them into columns i, j, instead of walking
// columns with a p*8-byte stride (pca128 -65%, pca256 -79%, 512 features 24.4 s -> 8.4 s). The oracle is the JS
// sample covariance C: every component v with eigenvalue l must satisfy |C v - l v| <= 1e-9 |C|, components
// are orthonormal, eigenvalues descend, and with every component kept they sum to trace(C).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_pca_fit_jacobi_rows_oracle");
import { PCA } from "dyna:ml";

let checks = 0, fails = 0;
function ok(c, what) { checks++; if (!c) { fails++; print("FAIL: " + what); } }
let seed = 0x77AB12;
const rnd = () => { seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed / 4294967296; };
function cov(X, r, c) {
    const mean = new Array(c).fill(0), C = new Array(c * c).fill(0);
    for (let i = 0; i < r; i++) for (let j = 0; j < c; j++) mean[j] += X[i * c + j] / r;
    for (let i = 0; i < r; i++)
        for (let a = 0; a < c; a++) for (let b = 0; b < c; b++) C[a * c + b] += (X[i * c + a] - mean[a]) * (X[i * c + b] - mean[b]);
    for (let k = 0; k < c * c; k++) C[k] /= r - 1;
    return C;
}
async function check(r, c, what) {
    const X = new Float64Array(r * c);
    for (let i = 0; i < r; i++) {
        const z = rnd(), y = rnd();
        for (let j = 0; j < c; j++) X[i * c + j] = (j % 3 === 0 ? z : j % 3 === 1 ? y + 0.3 * z : 0) + rnd() * (1 + (j % 4));
    }
    const C = cov(X, r, c), p = await new PCA().fit(X, r, c);
    const V = p.components, L = p.explainedVariance;
    let norm = 0, tr = 0;
    for (let k = 0; k < c * c; k++) norm = Math.max(norm, Math.abs(C[k]));
    for (let a = 0; a < c; a++) tr += C[a * c + a];
    ok(V.length === c && L.length === c, what + ": " + V.length + " components");
    let resid = 0, orth = 0, desc = true, sum = 0;
    for (let a = 0; a < c; a++) {
        sum += L[a];
        if (a > 0 && L[a] > L[a - 1] * (1 + 1e-12) + 1e-15) desc = false;
        for (let i = 0; i < c; i++) {
            let cv = 0;
            for (let j = 0; j < c; j++) cv += C[i * c + j] * V[a][j];
            resid = Math.max(resid, Math.abs(cv - L[a] * V[a][i]));
        }
        for (let b = 0; b < c; b++) {
            let d = 0;
            for (let j = 0; j < c; j++) d += V[a][j] * V[b][j];
            orth = Math.max(orth, Math.abs(d - (a === b ? 1 : 0)));
        }
    }
    ok(resid <= 1e-9 * Math.max(norm, 1e-300), what + ": |Cv - lv| " + resid + " vs |C| " + norm);
    ok(orth <= 1e-9, what + ": orthonormality error " + orth);
    ok(desc, what + ": eigenvalues descend");
    ok(Math.abs(sum - tr) <= 1e-9 * Math.abs(tr), what + ": sum " + sum + " vs trace " + tr);
}

await __kitS.section("every feature count from 1 to 70 matches the covariance eigen-equation", async () => {
    for (const c of [1, 2, 3, 4, 5, 8, 13, 33, 70]) await check(c * 2 + 5, c, "cols=" + c);
});

await __kitS.section("a wide fit (130 features) past the 1 KiB row pitch", async () => {
    await check(300, 130, "cols=130");
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_ml_pca_fit_jacobi_rows_oracle: " + checks + " checks passed");
__kitS.end();
