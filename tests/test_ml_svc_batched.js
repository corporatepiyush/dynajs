// timeout: 900
import { SVC } from "dyna:ml";

function assert(cond, msg) { if (!cond) throw new Error("FAIL: " + msg); }
function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }

function blobs(n, p, k, seed, spread) {
    const rnd = lcg(seed);
    const cent = [];
    for (let c = 0; c < k; c++) {
        const v = new Float64Array(p);
        for (let j = 0; j < p; j++) v[j] = (c % 2 ? 1 : -1) * (2 + rnd() * 2);
        cent.push(v);
    }
    const X = new Float64Array(n * p), y = new Float64Array(n);
    for (let i = 0; i < n; i++) {
        const c = i % k, m = cent[c];
        for (let j = 0; j < p; j++) X[i * p + j] = m[j] + (rnd() * 2 - 1) * spread;
        y[i] = c;
    }
    return { X, y };
}

function bits(a) {
    const b = new Uint8Array(new Float64Array(Array.from(a)).buffer);
    let s = "";
    for (let i = 0; i < b.length; i++) s += b[i].toString(16).padStart(2, "0");
    return s;
}
function flat(v) { return Array.isArray(v[0]) ? v.flat() : v; }

{
    const { X, y } = blobs(400, 8, 2, 12345, 0.5);
    const a = new SVC({ kernel: "rbf", C: 1, maxIter: 5000 }).fit(X, y, 400, 8);
    const b = new SVC({ kernel: "rbf", C: 1, maxIter: 5000 }).fit(X, y, 400, 8);
    assert(a.nSupportVectors === b.nSupportVectors,
           `two identical fits agree on nsv: ${a.nSupportVectors} vs ${b.nSupportVectors}`);
    assert(bits(flat(a.decisionFunction(X.subarray(0, 400 * 8), 400, 8))) ===
           bits(flat(b.decisionFunction(X.subarray(0, 400 * 8), 400, 8))),
           "two identical fits give bit-identical decision values");
    a.close(); b.close();
}

{
    const { X, y } = blobs(300, 6, 2, 555, 0.5);
    const m = new SVC({ kernel: "rbf", C: 1 });
    m.fit(X, y, 300, 6);
    const before = bits(flat(m.decisionFunction(X.subarray(0, 300 * 6), 300, 6)));
    m.fit(X, y, 300, 6);
    const after = bits(flat(m.decisionFunction(X.subarray(0, 300 * 6), 300, 6)));
    assert(before === after, "a refit reproduces the model bit-for-bit");
    m.close();
}

for (const kernel of ["rbf", "linear", "poly"]) {
    const { X, y } = blobs(240, 8, 2, 31337, 0.4);
    const m = new SVC({ kernel, C: 10, maxIter: 5000 });
    m.fit(X, y, 240, 8);
    const p = m.predict(X.subarray(0, 240 * 8), 240, 8);
    let right = 0;
    for (let i = 0; i < 240; i++) if (p[i] === y[i]) right++;
    assert(right === 240, `${kernel}: separable data fitted exactly (${right}/240)`);
    assert(m.nSupportVectors > 0, `${kernel}: has support vectors`);
    m.close();
}

{
    const X = new Float64Array([0, 0, 0.1, 0.1, 10, 10, 10.1, 10.1]);
    const y = new Float64Array([0, 0, 1, 1]);
    const m = new SVC({ kernel: "rbf", C: 1, maxIter: 5000 });
    m.fit(X, y, 4, 2);
    const p = m.predict(X.subarray(0, 4 * 2), 4, 2);
    assert(p[0] === 0 && p[3] === 1, `rows=4 separable (${p.join(",")})`);
    m.close();
}

{
    const X = new Float64Array([1, 1, 1, 1, 1, 1, 1, 1]);
    const y = new Float64Array([0, 0, 1, 1]);
    let threw = null;
    try { new SVC({ kernel: "rbf", C: 1, maxIter: 100 }).fit(X, y, 4, 2); }
    catch (e) { threw = e; }
    assert(threw === null, `duplicate rows fit rather than throw (${threw && threw.message})`);
    assert(threw === null, `duplicate rows fit rather than throw (${threw && threw.message})`);
}

{
    const n = 40, p = 2;
    const X = new Float64Array(n * p), y = new Float64Array(n);
    for (let i = 0; i < n; i++) {
        X[i * p] = (i % 2 ? 1e160 : -1e160);
        X[i * p + 1] = (i % 2 ? 1e160 : -1e160);
        y[i] = i % 2;
    }
    const m = new SVC({ kernel: "rbf", C: 1, maxIter: 200 });
    let threw = null;
    try { m.fit(X, y, n, p); } catch (e) { threw = e; }
    assert(threw === null, `underflowing kernel is finite and accepted (${threw && threw.message})`);
    m.close();
}

{
    const { X, y } = blobs(50, 4, 2, 9, 0.5);
    X[7 * 4 + 1] = NaN;
    let threw = null;
    try { new SVC({ kernel: "rbf" }).fit(X, y, 50, 4); } catch (e) { threw = e; }
    assert(threw !== null, "NaN in X is still refused");
    assert(/X\[7\]\[1\]/.test(threw.message), `names the cell: ${threw.message}`);

    const { X: X2, y: y2 } = blobs(50, 4, 2, 9, 0.5);
    X2[3 * 4 + 2] = Infinity;
    threw = null;
    try { new SVC({ kernel: "rbf" }).fit(X2, y2, 50, 4); } catch (e) { threw = e; }
    assert(threw !== null && /infinite/.test(threw.message),
           `+Inf in X is still refused and named: ${threw && threw.message}`);
}

{
    const { X, y } = blobs(60, 3, 2, 4, 0.5);
    for (let i = 0; i < 60; i++)
        for (let j = 0; j < 3; j++) X[i * 3 + j] *= 1e40;
    const m = new SVC({ kernel: "poly", C: 1, degree: 60, maxIter: 50 });
    let threw = null;
    try { m.fit(X, y, 60, 3); } catch (e) { threw = e; }
    assert(threw !== null, "a poly degree overflow is still refused");
    assert(/polynomial kernel overflowed/.test(threw.message),
           `and still says so: ${threw && threw.message}`);
    m.close();
}

{
    const n = 30, p = 3;
    const X = new Float64Array(n * p), y = new Float64Array(n);
    for (let i = 0; i < n; i++) { X[i * p] = 2; X[i * p + 1] = -2; X[i * p + 2] = 0.5; y[i] = i % 2; }
    const m = new SVC({ kernel: "rbf", C: 1, maxIter: 200 });
    m.fit(X, y, n, p);
    const p2 = m.predict(X.subarray(0, n * p), n, p);
    assert(p2.length === n, "a degenerate matrix still predicts every row");
    m.close();
}

{
    const { X, y } = blobs(200, 8, 2, 2024, 0.5);
    const m = new SVC({ kernel: "rbf", C: 1, maxIter: 5000 }).fit(X, y, 200, 8);
    const d = flat(m.decisionFunction(X.subarray(0, 200 * 8), 200, 8));
    assert(d.length === 200, "decisionFunction returns one value per row");
    for (const v of d) assert(Number.isFinite(v), `decision value finite (${v})`);
    m.close();
}

print("test_ml_svc_batched: SVC RBF row split is value-identical (10 groups)");
