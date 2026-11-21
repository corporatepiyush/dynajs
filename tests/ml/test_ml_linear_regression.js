// covers: dyna:ml.LinearRegression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_linear_regression");
import { LinearRegression, LogisticRegression, KMeans, SVC, trainTestSplit, kFold, gridSearch } from "dyna:ml";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function approx(a, b, eps, msg) {
    n++;
    if (Math.abs(a - b) > eps)
        throw new Error("approx failed: " + msg + " (" + a + " vs " + b + ")");
}

await __kitS.section("fit returns this", async () => {
    const m = new LinearRegression();
    try {
        const X = [], y = [];
        for (let x = 0; x < 20; x++) { X.push([x]); y.push(2 * x + 1); }
        assert((await m.fit(X, y)) === m, "fit returns this");
        const p = await m.predict([[100], [0], [-3]]);
        assert(Array.isArray(p) && p.length === 3, "predict returns Array of 3");
        approx(p[0], 201, 1e-4, "y(100)=201");
        approx(p[1], 1, 1e-4, "y(0)=1");
        approx(p[2], -5, 1e-4, "y(-3)=-5");
    } finally { m.close(); }
});

await __kitS.section("plane at (10,10)", async () => {
    const m = new LinearRegression();
    try {
        const X = [], y = [];
        for (let a = 0; a < 6; a++)
            for (let b = 0; b < 6; b++) { X.push([a, b]); y.push(3 * a - 2 * b + 5); }
        await m.fit(X, y);
        const p = await m.predict([[10, 10], [0, 0]]);
        approx(p[0], 3 * 10 - 2 * 10 + 5, 1e-3, "plane at (10,10)");
        approx(p[1], 5, 1e-3, "plane intercept");
    } finally { m.close(); }
});

await __kitS.section("y length != rows throws", async () => {
    const m = new LinearRegression();
    try {
        let threw = false;
        try { await m.fit([[1], [2]], [1]); } catch { threw = true; }
        assert(threw, "y length != rows throws");
        threw = false;
        try { await m.fit([[1, 2], [3]], [1, 2]); } catch { threw = true; }
        assert(threw, "ragged X rows throw");
        await m.fit([[1], [2], [3]], [3, 5, 7]);
        threw = false;
        try { await m.predict([[1, 2]]); } catch { threw = true; }
        assert(threw, "predict feature-count mismatch throws");
        threw = false;
        try { await m.fit([], []); } catch { threw = true; }
        assert(threw, "empty X throws");
    } finally { m.close(); }
});

await __kitS.section("open initially", async () => {
    const m = new LinearRegression();
    assert(m.closed === false, "open initially");
    await m.fit([[1], [2], [3]], [2, 4, 6]);
    m.close();
    assert(m.closed === true, "closed after close()");
    let threw = false;
    try { await m.predict([[4]]); } catch { threw = true; }
    assert(threw, "use-after-close throws");
    m.close();
    assert(m.closed === true, "still closed after 2nd close()");
});

await __kitS.section("linreg.fit coerce-then-close is caught (no UAF)", async () => {
    const m = new LinearRegression();
    let threw = false;
    try {
        await m.fit([[{ valueOf() { m.close(); return 1; } }]], [1]);
    } catch { threw = true; }
    assert(threw, "linreg.fit coerce-then-close is caught (no UAF)");
    m.close();
});

await __kitS.section("flat fit returns this", async () => {
    const R = 20, C = 1;
    const Xf = new Float64Array(R * C), yf = new Float64Array(R);
    for (let x = 0; x < R; x++) { Xf[x] = x; yf[x] = 2 * x + 1; }
    const m = new LinearRegression();
    try {
        assert((await m.fit(Xf, yf, R, C)) === m, "flat fit returns this");
        const Xp = new Float64Array([100, 0, -3]);
        const p = await m.predict(Xp, 3, 1);
        approx(p[0], 201, 1e-4, "flat y(100)=201");
        approx(p[1], 1, 1e-4, "flat y(0)=1");
        approx(p[2], -5, 1e-4, "flat y(-3)=-5");
    } finally { m.close(); }
});

await __kitS.section("flat == AoA prediction", async () => {
    const R = 500, C = 6;
    let s = 99;
    const rnd = () => { s = (s * 1103515245 + 12345) & 0x7fffffff; return s / 0x7fffffff * 2 - 1; };
    const w = []; for (let j = 0; j < C; j++) w.push(rnd());
    const Xaoa = [], yaoa = [], Xf = new Float64Array(R * C), yf = new Float64Array(R);
    for (let i = 0; i < R; i++) {
        const row = []; let t = 0.7;
        for (let j = 0; j < C; j++) { const v = rnd(); row.push(v); Xf[i * C + j] = v; t += v * w[j]; }
        Xaoa.push(row); yaoa.push(t); yf[i] = t;
    }
    const Xaof = []; for (let i = 0; i < R; i++) Xaof.push(Xf.subarray(i * C, i * C + C));

    const mA = new LinearRegression(), mF = new LinearRegression(), mV = new LinearRegression();
    try {
        await mA.fit(Xaoa, yaoa);
        await mF.fit(Xf, yf, R, C);
        await mV.fit(Xaof, yaoa);
        const q = [[1, 2, 3, 4, 5, 6], [-1, 0, 1, -2, 0, 2]];
        const qf = new Float64Array([1, 2, 3, 4, 5, 6, -1, 0, 1, -2, 0, 2]);
        const pA = await mA.predict(q), pF = await mF.predict(qf, 2, C), pV = await mV.predict(Xaof.slice(0, 2).map((_, i) => Xaof[i]));
        for (let i = 0; i < 2; i++) {
            approx(pF[i], pA[i], 1e-9, "flat == AoA prediction " + i);
        }
        const pV2 = await mV.predict(q);
        for (let i = 0; i < 2; i++)
            approx(pV2[i], pA[i], 1e-9, "AoF64 == AoA prediction " + i);
    } finally { mA.close(); mF.close(); mV.close(); }
});

await __kitS.section("flat X without (rows,cols) throws", async () => {
    const m = new LinearRegression();
    try {
        const Xf = new Float64Array(6);
        let threw = false;
        try { await m.fit(Xf, new Float64Array(3)); } catch { threw = true; }
        assert(threw, "flat X without (rows,cols) throws");
        threw = false;
        try { await m.fit(Xf, new Float64Array(3), 3, 3); } catch { threw = true; }
        assert(threw, "flat length != rows*cols throws");
        threw = false;
        try { await m.fit(Xf, new Float64Array(2), 3, 2); } catch { threw = true; }
        assert(threw, "flat y length mismatch throws");
    } finally { m.close(); }
});

await __kitS.section("flat fit close-during-shape-coerce is caught (no UAF)", async () => {
    const m = new LinearRegression();
    const Xf = new Float64Array([1, 2, 3]);
    let threw = false;
    try {
        await m.fit(Xf, new Float64Array([1, 2, 3]),
              { valueOf() { m.close(); return 3; } }, 1);
    } catch { threw = true; }
    assert(threw, "flat fit close-during-shape-coerce is caught (no UAF)");
    m.close();
});
__kitS.end();

print("test_ml: all tests passed (" + n + " assertions)");
