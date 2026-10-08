// timeout: 300
import { KMeans, KNClassifier, StandardScaler, MinMaxScaler, LinearRegression,
         LogisticRegression, PCA, GaussianNB, SVC, GaussianMixture,
         DecisionTreeClassifier, DecisionTreeRegressor,
         RandomForestClassifier, RandomForestRegressor,
         GradientBoostingClassifier, GradientBoostingRegressor,
         XGBClassifier, XGBRegressor } from "dyna:ml";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };

const CLASS = [
    ["+0",                  "0000000000000000", true],
    ["-0",                  "8000000000000000", true],
    ["+1",                  "3ff0000000000000", true],
    ["-1",                  "bff0000000000000", true],
    ["smallest subnormal",  "0000000000000001", true],
    ["-smallest subnormal", "8000000000000001", true],
    ["1e-320",              "00000000000007e8", true],
    ["smallest NORMAL",     "0010000000000000", true],
    ["-smallest NORMAL",    "8010000000000000", true],
    ["DBL_MAX",             "7fefffffffffffff", true],
    ["-DBL_MAX",            "ffefffffffffffff", true],
    ["1e308",               "7fe1ccf385ebc8a0", true],
    ["1e-308",              "000730d67819e8d2", true],
    ["2^53",                "4340000000000000", true],
    ["+Inf",                "7ff0000000000000", false],
    ["-Inf",                "fff0000000000000", false],
    ["NaN",                 "7ff8000000000000", false],
    ["-NaN",                "fff8000000000000", false],
];

function fromBits(hex) {
    const b = new Uint8Array(8);
    for (let i = 0; i < 8; i++) b[i] = parseInt(hex.substr(14 - 2 * i, 2), 16);
    const a = new Float64Array(1);
    a[0] = new DataView(b.buffer).getFloat64(0, true);
    return a[0];
}

const cIsFinite   = v => v === v && v !== Infinity && v !== -Infinity;
const cBranchless = v => (v > -Infinity && v < Infinity);
const cMissingOk  = v => Math.abs(v) === Infinity;

const X = [[0, 0], [0, 1], [1, 0], [1, 1], [0.2, 0.8], [0.8, 0.2], [0.3, 0.3], [0.7, 0.7]];
const yr = [0.1, 0.2, 0.2, 0.3, 0.15, 0.25, 0.22, 0.28];
const yc = [0, 0, 1, 1, 0, 1, 0, 1];

print("== 1. the three forms agree with numpy, on every class of double ==");
for (const [name, bits, finite] of CLASS) {
    const v = fromBits(bits);
    ok(cIsFinite(v) === finite, "isfinite(" + name + ") == numpy " + finite);
    ok(cBranchless(v) === finite, "branchless(" + name + ") == numpy " + finite);
    ok(cIsFinite(v) === cBranchless(v), "old and new forms agree on " + name);
    ok(cMissingOk(v) === (!finite && v === v), "missing_ok arm on " + name);
}

print("");
print("== 2. every fit path, poisoned X, accepts exactly the finite values ==");
function fitVerdict(make, v) {
    const Xb = X.map(r => r.slice());
    Xb[1][1] = v;
    const yb = yr.slice(); yb[3] = v;
    try { const m = make(); m.fit(Xb, yb); m.close(); return "accepted"; }
    catch (e) {
        const msg = String(e.message);
        if (msg.indexOf("X[1][1]") >= 0) return "refused:X";
        if (msg.indexOf("y[3]") >= 0) return "refused:y";
        return "refused-other:" + msg.slice(0, 40);
    }
}

const FITS = [
    ["KMeans",                   () => new KMeans(2, 1), false, false],
    ["GaussianMixture",          () => new GaussianMixture(2, { seed: 1 }), false, false],
    ["GaussianNB",               () => new GaussianNB(), false, true],
    ["SVC",                      () => new SVC({ kernel: "rbf", maxIter: 20 }), false, true],
    ["LinearRegression",         () => new LinearRegression(), false, true],
    ["LogisticRegression",       () => new LogisticRegression({ maxIter: 20 }), false, true],
    ["DecisionTreeClassifier",   () => new DecisionTreeClassifier({ seed: 1 }), false, true],
    ["DecisionTreeRegressor",    () => new DecisionTreeRegressor({ seed: 1 }), false, true],
    ["RandomForestClassifier",   () => new RandomForestClassifier({ nEstimators: 2, seed: 1 }), false, true],
    ["RandomForestRegressor",    () => new RandomForestRegressor({ nEstimators: 2, seed: 1 }), false, true],
    ["GradientBoostingClassifier", () => new GradientBoostingClassifier({ nEstimators: 2, seed: 1 }), false, true],
    ["GradientBoostingRegressor",  () => new GradientBoostingRegressor({ nEstimators: 2, seed: 1 }), false, true],
    ["XGBClassifier",            () => new XGBClassifier({ nEstimators: 2, seed: 1 }), true, true],
    ["XGBRegressor",             () => new XGBRegressor({ nEstimators: 2, seed: 1 }), true, true],
];

for (const [name, make, missingOk, hasTarget] of FITS) {
    for (const [label, bits, finite] of CLASS) {
        const v = fromBits(bits);
        const got = fitVerdict(make, v);
        if (finite) {
            ok(got !== "refused:X" && got !== "refused:y",
               name + " fit X[1][1]=y[3]=" + label + " -> the finite check does not fire",
               "got " + got);
            continue;
        }
        let want;
        if (missingOk && v !== v) want = hasTarget ? "refused:y" : "accepted";
        else want = "refused:X";
        ok(got === want, name + " fit X[1][1]=y[3]=" + label + " -> " + want, "got " + got);
    }
}

print("");
print("== 3. the poisoning VALUE never changes WHICH cell is named ==");
for (const [name, make, missingOk, hasTarget] of FITS) {
    if (missingOk) continue;
    for (const [row, col, where] of [[0, 0, "first cell"], [7, 1, "last cell"],
                                     [3, 1, "middle"]]) {
        const Xb = X.map(r => r.slice());
        Xb[row][col] = NaN;
        let msg = "";
        try { const m = make(); m.fit(Xb, yr); m.close(); } catch (e) { msg = String(e.message); }
        ok(msg.indexOf("X[" + row + "][" + col + "]") >= 0,
           name + " names X[" + row + "][" + col + "] (" + where + ")", msg.slice(0, 60));
    }
    for (const [label, v] of [["+Inf", Infinity], ["-Inf", -Infinity]]) {
        const Xi = X.map(r => r.slice());
        Xi[2][1] = v;
        let msg = "";
        try { const m = make(); m.fit(Xi, yr); m.close(); } catch (e) { msg = String(e.message); }
        ok(msg.indexOf("X[2][1]") >= 0 && msg.indexOf("is infinite") >= 0,
           name + " names an " + label + " as infinite", msg.slice(0, 60));
    }
    if (hasTarget) {
        const yb = yr.slice(); yb[5] = Infinity;
        let msg = "";
        try { const m = make(); m.fit(X, yb); m.close(); } catch (e) { msg = String(e.message); }
        ok(msg.indexOf("y[5]") >= 0, name + " names y[5]", msg.slice(0, 60));
    }
}

print("");
print("== 4. y is never missing_ok: a target cannot be imputed ==");
for (const [name, make, missingOk, hasTarget] of FITS) {
    if (!hasTarget) continue;
    for (const [label, bits, finite] of CLASS) {
        const yb = yr.slice(); yb[3] = fromBits(bits);
        let got = "accepted";
        try { const m = make(); m.fit(X, yb); m.close(); } catch (e) {
            got = /y\[3\]/.test(String(e.message)) ? "refused:y" : "refused-other";
        }
        const want = finite ? "accepted" : "refused:y";
        ok(got === want, name + " fit y[3]=" + label + " -> " + want, "got " + got);
    }
}

print("");
print("== 5. the predict path keeps its own, different message ==");
{
    const REFUSES = [
        ["KMeans", new KMeans(2, 1).fit(X), (m, Xp) => m.predict(Xp)],
        ["KNClassifier", new KNClassifier(3).fit(X, yc), (m, Xp) => m.predict(Xp)],
    ];
    for (const [name, m, call] of REFUSES) {
        for (const [label, bits, finite] of CLASS) {
            const Xp = X.map(r => [r[0], fromBits(bits)]);
            let got = "accepted";
            try { call(m, Xp); } catch (e) { got = String(e.message); }
            if (finite) {
                ok(got === "accepted", name + " predict " + label + " accepted", got.slice(0, 50));
            } else {
                ok(got === "ml.predict: predict input contains NaN or infinite values",
                   name + " predict " + label + " refused with the predict message",
                   got.slice(0, 60));
            }
        }
        m.close();
    }
    const GAPS = [
        ["StandardScaler.transform", new StandardScaler().fit(X), (m, Xp) => m.transform(Xp)],
        ["PCA.transform", new PCA(1).fit(X), (m, Xp) => m.transform(Xp)],
    ];
    for (const [name, m, call] of GAPS) {
        for (const [label, bits, finite] of CLASS) {
            const Xp = X.map(r => [r[0], fromBits(bits)]);
            let got = "accepted";
            try { call(m, Xp); } catch (e) { got = String(e.message); }
            if (finite) ok(got === "accepted", name + " " + label + " accepted", got.slice(0, 50));
            else ok(got === "accepted",
                    name + " " + label + " accepted (NO finite check on this path -- ML-07/ML-11, unfixed by design here)",
                    got.slice(0, 60));
        }
        m.close();
    }
}

print("");
print("== 6. the branchless pass does not perturb a single bit of the data ==");
{
    const R = 64, C = 8;
    const A = new Float64Array(R * C);
    for (let i = 0; i < R; i++)
        for (let j = 0; j < C; j++)
            A[i * C + j] = Math.sin(i * 0.37 + j * 1.11) * 10 + j;

    const before = A.slice();
    const s = new StandardScaler().fit(A, R, C);
    let same = true;
    for (let i = 0; i < A.length; i++) if (A[i] !== before[i]) same = false;
    ok(same, "the fit does not modify the caller's buffer");

    for (let j = 0; j < C; j++) {
        let m = 0;
        for (let i = 0; i < R; i++) m += A[i * C + j];
        m /= R;
        let v = 0;
        for (let i = 0; i < R; i++) { const d = A[i * C + j] - m; v += d * d; }
        v /= R;
        const sc = Math.sqrt(v);
        ok(Math.abs(s.mean[j] - m) <= 1e-12 * Math.max(1, Math.abs(m)),
           "StandardScaler.mean[" + j + "] matches an independent JS pass",
           s.mean[j] + " vs " + m);
        ok(Math.abs(s.std[j] - (sc > 0 ? sc : 1)) <= 1e-12 * Math.max(1, sc),
           "StandardScaler.std[" + j + "] matches an independent JS pass",
           s.std[j] + " vs " + sc);
    }
    s.close();

    const mm = new MinMaxScaler().fit(A, R, C);
    for (let j = 0; j < C; j++) {
        let lo = Infinity, hi = -Infinity;
        for (let i = 0; i < R; i++) {
            if (A[i * C + j] < lo) lo = A[i * C + j];
            if (A[i * C + j] > hi) hi = A[i * C + j];
        }
        ok(Math.abs(mm.dataMin[j] - lo) <= 1e-12, "MinMaxScaler.dataMin[" + j + "]");
        ok(Math.abs(mm.dataMax[j] - hi) <= 1e-12, "MinMaxScaler.dataMax[" + j + "]");
    }
    mm.close();
}

print("");
print("test_ml_finite_scan: " +
      (fail ? "FAILURES: " + fail + " / " : "all ") + (pass + fail) +
      " assertions passed");
if (fail)
    throw new Error("test_ml_finite_scan: " + fail + " of " + (pass + fail) +
                    " assertions FAILED");
