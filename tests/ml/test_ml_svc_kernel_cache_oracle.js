// covers: dyna:ml.SVC
// SVC training (SMO) keeps each kernel row K(i, *) it computes, up to 64 MiB per binary sub-problem, and reuses it on
// later steps that touch row i again instead of recomputing rows*cols kernel terms (one exp each for rbf). A row is
// computed with the same call as before, so every alpha, bias, prediction and decision value is bit-identical; past
// the budget rows are computed into the scratch buffers as before. The overflow check (poly kernel) still runs on
// every freshly computed row and throws the same RangeError. Oracles: predictions and decision functions recorded from
// the binary before the cache, for rbf, linear, poly and a three-class rbf at 300 and 1500 rows; a separable problem
// classified perfectly; the overflow error.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_svc_kernel_cache_oracle");
import * as ml from "dyna:ml";
import { platform } from "dyna:sys";
// The recorded fingerprints come from macOS (Apple libm, clang); glibc exp/lgamma and libc tie orders differ in the last bit, so those sections run only where they were recorded. The same-binary oracles run everywhere.
const RECORDED_HERE = platform() === "darwin";

let checks = 0, fails = 0;
function eq(got, want, what) {
    checks++;
    if (got !== want) { fails++; if (fails < 20) print("FAIL: " + what + ": got " + got + ", want " + want); }
}
const hash = (arr) => { const b = new Float64Array(Array.from(arr)); const u = new Int32Array(b.buffer); let x = 0; for (let i = 0; i < u.length; i++) x = (Math.imul(x, 31) + u[i]) | 0; return x; };

const before = {
    300: "rbf -756023296 -510775007|lin 1590689792 -2122273694|poly -2026897408 -1892210778|rbf3 1854930944 1052397665",
    1500: "rbf -748683264 864693807|lin 176160768 -1633042831|poly -425721856 -972658115|rbf3 779091968 2569520",
};
await __kitS.section("predictions and decision values equal the uncached build", async () => {
    if (!RECORDED_HERE) { print("skip: fingerprints recorded on darwin, running on " + platform()); return; }
    for (const N of [300, 1500]) {
        const C = 5;
        let seed = 5;
        const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);
        const X = new Float64Array(N * C), y = new Float64Array(N), y3 = new Float64Array(N);
        for (let i = 0; i < N; i++) {
            let a = 0;
            for (let j = 0; j < C; j++) { const v = rnd() * 4 - 2; X[i * C + j] = v; a += v * (j % 2 ? 1 : -1); }
            y[i] = a + (rnd() - 0.5) > 0 ? 1 : 0; y3[i] = (Math.floor(a * 2) % 3 + 3) % 3;
        }
        const want = before[N].split("|");
        const cases = [["rbf", { kernel: "rbf", maxIter: 50 }, y], ["lin", { kernel: "linear", maxIter: 50 }, y], ["poly", { kernel: "poly", degree: 3, maxIter: 30 }, y], ["rbf3", { kernel: "rbf", C: 2, maxIter: 30 }, y3]];
        for (let c = 0; c < cases.length; c++) {
            const [nm, o, yy] = cases[c];
            const m = await new ml.SVC(o).fit(X, yy, N, C);
            const got = nm + " " + hash(await m.predict(X, N, C)) + " " + hash(Array.from(await m.decisionFunction(X, N, C)).flat());
            m.close();
            eq(got, want[c], "N=" + N + " " + nm);
        }
    }
});

await __kitS.section("a separable problem is classified perfectly", async () => {
    const N = 400, C = 2, X = new Float64Array(N * C), y = new Float64Array(N);
    for (let i = 0; i < N; i++) { const s = i % 2 ? 1 : -1; X[i * C] = s * (1 + (i % 17) / 10); X[i * C + 1] = ((i * 7) % 23) / 5 - 2; y[i] = i % 2; }
    for (const kernel of ["rbf", "linear"]) {
        const m = await new ml.SVC({ kernel }).fit(X, y, N, C);
        const p = await m.predict(X, N, C);
        m.close();
        let wrong = 0;
        for (let i = 0; i < N; i++) if (p[i] !== y[i]) wrong++;
        eq(wrong, 0, kernel + " misclassified rows");
    }
});

await __kitS.section("an overflowing poly kernel still throws", async () => {
    const N = 50, C = 3, X = new Float64Array(N * C), y = new Float64Array(N);
    for (let i = 0; i < N * C; i++) X[i] = 1e40 * ((i % 7) + 1);
    for (let i = 0; i < N; i++) y[i] = i % 2;
    let msg = "no throw";
    try { await new ml.SVC({ kernel: "poly", degree: 9 }).fit(X, y, N, C); } catch (e) { msg = e.constructor.name + ": " + e.message; }
    eq(msg, "RangeError: ml.fit: polynomial kernel overflowed; scale X or lower degree", "overflow error");
    const Xs = new Float64Array(N * C);
    for (let i = 0; i < N * C; i++) Xs[i] = ((i % 5) - 2) * 0.1;
    for (let j = 0; j < C; j++) Xs[(N - 1) * C + j] = 1e40;
    // row N-1 shares row 0's label, so the first SMO pair never puts it on the diagonal: only a kernel row overflows
    const ys = Float64Array.from(y);
    ys[N - 1] = ys[0];
    msg = "no throw";
    try { await new ml.SVC({ kernel: "poly", degree: 9, gamma: 1, maxIter: 5 }).fit(Xs, ys, N, C); } catch (e) { msg = e.constructor.name + ": " + e.message; }
    eq(msg, "RangeError: ml.fit: polynomial kernel overflowed; scale X or lower degree", "overflow in a kernel row only");
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_ml_svc_kernel_cache_oracle: " + checks + " checks passed");
__kitS.end();
