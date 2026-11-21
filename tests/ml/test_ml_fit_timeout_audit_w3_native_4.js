// timeout: 300
// covers: integration dyna:ml.LogisticRegression dyna:ml.RandomForestClassifier dyna:ml.GradientBoostingClassifier dyna:ml.SVC dyna:ml.GaussianMixture -- every training kernel obeys --timeout-ms, one child per kernel
// tests/ml/test_ml_fit_timeout_audit_w3_native_4.js -- audit wave 3, batch 4 (the
// ML half; the Redis N3-07 mock section lives in
// tests/redis/test_redis_redis_audit_w3_native_4.js).
// Each section names its ORACLE and CONTROL; [red] fails on the pre-fix binary.
//   M1b-02 model fitting obeys --timeout-ms (one case per training kernel)
// build-note: needs the native modules
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_fit_timeout_audit_w3_native_4");
import { args, Exec } from "dyna:sys";
import { hostScale } from "../lib/hostscale.js";
import { Path, makeTempDir, writeFile, removeAll } from "dyna:file";

const BIN = args()[0];
let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; print("  FAIL:", msg); } }
const TMP = makeTempDir("w3n4-");
const P = (n) => new Path(TMP.toString() + "/" + n);
function child(name, src, flags = []) {
    writeFile(P(name), src);
    return Exec(BIN, [...flags, P(name).toString()], { timeoutMs: 60000, encoding: "utf8" });
}

// ---- M1b-02 -------------------------------------------------------------
// ORACLE: the documented exit shapes. A fit that outlives --timeout-ms must
// end with the engine's "interrupted" InternalError (status 1). Status 113
// is the hard-stop watchdog, which means no code in the training loop ever
// looked at the deadline. Each fit below is sized to run for many seconds.
// CONTROL: the same model fitted on 40 rows under the same flag returns.

await __kitS.section("const rows = 20000, cols = 20; const X = new Float64Array(rows * cols), y = new", async () => {
    const data = `
const rows = 20000, cols = 20;
const X = new Float64Array(rows * cols), y = new Float64Array(rows);
let s = 1; for (let i = 0; i < X.length; i++) { s = (s * 1103515245 + 12345) & 0x7fffffff; X[i] = s / 0x7fffffff; }
for (let i = 0; i < rows; i++) y[i] = X[i * cols] + X[i * cols + 1] > 1 ? 1 : 0;
const Xs = X.subarray(0, 40 * cols), ys = y.subarray(0, 40);
`;
    const families = [
        ["LogisticRegression", `await new ml.LogisticRegression({ maxIter: 50 }).fit(Xs, ys, 40, cols)`, `await new ml.LogisticRegression({ maxIter: 100000, tol: 0 }).fit(X, y, rows, cols)`],
        ["RandomForestClassifier", `await new ml.RandomForestClassifier({ nEstimators: 3, seed: 1 }).fit(Xs, ys, 40, cols)`, `await new ml.RandomForestClassifier({ nEstimators: 20000, seed: 1 }).fit(X, y, rows, cols)`],
        ["GradientBoostingClassifier", `await new ml.GradientBoostingClassifier({ nEstimators: 3, seed: 1 }).fit(Xs, ys, 40, cols)`, `await new ml.GradientBoostingClassifier({ nEstimators: 20000, seed: 1 }).fit(X, y, rows, cols)`],
        ["SVC", `await new ml.SVC({ maxIter: 5 }).fit(Xs, ys, 40, cols)`, `const yn = y.slice(0, 4000); for (let i = 0; i < 4000; i++) yn[i] = (i * 7919 % 13) < 6 ? 1 : 0; await new ml.SVC({ kernel: "rbf", C: 1000, maxIter: 100000, tol: 1e-15 }).fit(X.subarray(0, 4000 * cols), yn, 4000, cols)`],
        ["GaussianMixture", `await new ml.GaussianMixture(2, { seed: 1, maxIter: 5 }).fit(Xs, 40, cols)`, `await new ml.GaussianMixture(8, { seed: 1, maxIter: 10000, tol: 0 }).fit(X, rows, cols)`],
    ];
    for (const [name, small, big] of families) {
        const r = child("fit_" + name + ".mjs", `import * as ml from "dyna:ml";${data}
${small}; print("small-ok");
${big}; print("big-finished");
`, ["--timeout-ms", String(Math.round(500 * hostScale()))]);
        ok(/small-ok/.test(r.stdout), "control: a small " + name + " fit runs under --timeout-ms (" + (r.stdout + r.stderr).slice(0, 120) + ")");
        ok(!/big-finished/.test(r.stdout), name + ": the long fit did not run to completion");
        ok(/interrupted/.test(r.stderr) && r.code === 1, "[red] M1b-02: " + name + ".fit is interrupted by the deadline (code " + r.code + ", " + r.stderr.split("\n")[0].slice(0, 70) + ")");
    }
});

__kitS.end();

try { removeAll(TMP); } catch (e) {}
print("test_audit_w3_native_4: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_native_4: " + failures + " failures");
