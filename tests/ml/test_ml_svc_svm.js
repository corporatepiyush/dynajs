// covers: dyna:ml.SVC
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_svc_svm");
import { SVC, GaussianMixture, accuracy } from "dyna:ml";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function approx(a, b, eps, msg) {
    n++;
    if (!(Math.abs(a - b) <= eps)) throw new Error(`approx failed: ${msg} (${a} vs ${b})`);
}
async function throws(fn, msg, ErrType) {
    n++;
    try { await fn(); } catch (e) {
        if (ErrType && !(e instanceof ErrType))
            throw new Error(`${msg}: wrong error type ${e.constructor.name}: ${e.message}`);
        return;
    }
    throw new Error("expected a throw: " + msg);
}
function rng(seed) {
    let s = seed >>> 0;
    return () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
}
function xorData(perQuadrant, seed) {
    const rnd = rng(seed);
    const X = [], y = [];
    for (const [cx, cy, lab] of [[0, 0, 0], [1, 1, 0], [0, 1, 1], [1, 0, 1]])
        for (let i = 0; i < perQuadrant; i++) {
            X.push([cx + (rnd() - 0.5) * 0.2, cy + (rnd() - 0.5) * 0.2]);
            y.push(lab);
        }
    return { X, y };
}

await __kitS.section("linear", async () => {
    const X = [[1, 1], [1.2, 0.9], [0.8, 1.1], [5, 5], [5.2, 4.8], [4.9, 5.1]];
    const y = [0, 0, 0, 1, 1, 1];
    for (const kernel of ["linear", "rbf", "poly"]) {
        const m = new SVC({ kernel, C: 10 });
        try {
            assert((await m.fit(X, y)) === m, `${kernel}: fit returns this`);
            approx(await accuracy(y, await m.predict(X)), 1, 0, `${kernel}: separable data fitted exactly`);
            const p = await m.predict([[1.1, 1], [5.1, 5]]);
            assert(p[0] === 0 && p[1] === 1, `${kernel}: unseen points classified`);
            assert(m.nSupportVectors > 0, `${kernel}: has support vectors`);
            assert(m.nSupportVectors <= X.length,
                   `${kernel}: no more support vectors than samples`);
            const cls = m.classes;
            assert(cls.length === 2 && cls[0] === 0 && cls[1] === 1,
                   `${kernel}: classes ascending`);
        } finally { m.close(); }
    }
});

await __kitS.section("binary decisionFunction is one value per row", async () => {
    const X = [[0], [1], [8], [9]];
    const y = [0, 0, 1, 1];
    const m = new SVC({ kernel: "linear", C: 10 });
    try {
        await m.fit(X, y);
        const q = [[0.5], [1.5], [7], [8.5], [4.5]];
        const d = await m.decisionFunction(q);
        const p = await m.predict(q);
        assert(Array.isArray(d) && d.length === 5, "binary decisionFunction is one value per row");
        for (let i = 0; i < q.length; i++)
            assert((d[i] >= 0 ? 1 : 0) === p[i],
                   `sign of the margin matches the label @${i} (d=${d[i]}, p=${p[i]})`);
        assert(d[0] < d[1] && d[1] < d[4] && d[4] < d[2] && d[2] < d[3],
               "margin increases towards the positive class");
    } finally { m.close(); }
});

await __kitS.section("RBF separates XOR", async () => {
    const { X, y } = xorData(15, 2024);
    const lin = new SVC({ kernel: "linear", C: 10 });
    const rbf = new SVC({ kernel: "rbf", C: 10, gamma: 1.0 });
    try {
        await lin.fit(X, y);
        await rbf.fit(X, y);
        const accLin = await accuracy(y, await lin.predict(X));
        const accRbf = await accuracy(y, await rbf.predict(X));
        assert(accRbf > 0.95, "RBF separates XOR: " + accRbf);
        assert(accLin < 0.8, "linear cannot separate XOR: " + accLin);
        assert(accRbf > accLin, "the kernel is what makes the difference");
    } finally { lin.close(); rbf.close(); }
});

await __kitS.section("three classes fitted exactly", async () => {
    const X = [[0, 0], [0.1, 0], [0, 0.1], [5, 5], [5.1, 5], [5, 5.1],
               [10, 0], [10.1, 0], [10, 0.1]];
    const y = [0, 0, 0, 1, 1, 1, 2, 2, 2];
    const m = new SVC({ kernel: "rbf", C: 100, gamma: 0.1 });
    try {
        await m.fit(X, y);
        approx(await accuracy(y, await m.predict(X)), 1, 0, "three classes fitted exactly");
        assert(m.classes.length === 3, "three classes discovered");
        const d = await m.decisionFunction([[0, 0], [5, 5]]);
        assert(Array.isArray(d) && Array.isArray(d[0]) && d[0].length === 3,
               "multi-class decisionFunction is rows x classes");
        const probe = [[0, 0], [5, 5]];
        const probePred = await m.predict(probe);
        for (let i = 0; i < probe.length; i++) {
            let best = 0;
            for (let c = 1; c < 3; c++) if (d[i][c] > d[i][best]) best = c;
            assert(probePred[i] === m.classes[best],
                   `argmax of the OvR values is the prediction @${i}`);
        }
    } finally { m.close(); }
});

await __kitS.section("negative fractional label preserved", async () => {
    const m = new SVC({ kernel: "linear", C: 10 });
    try {
        await m.fit([[0], [0.2], [9], [9.2]], [-2.5, -2.5, 7, 7]);
        const p = await m.predict([[0.1], [9.1]]);
        approx(p[0], -2.5, 0, "negative fractional label preserved");
        approx(p[1], 7, 0, "positive label preserved");
    } finally { m.close(); }
});

await __kitS.section("block L120", async () => {
    const { X, y } = xorData(10, 77);
    for (const C of [0.01, 1000]) {
        const m = new SVC({ kernel: "rbf", C, gamma: 1 });
        try {
            await m.fit(X, y);
            const p = await m.predict(X);
            assert(p.length === X.length, `C=${C} still predicts every row`);
            assert(p.every((v) => v === 0 || v === 1), `C=${C} predicts valid labels`);
        } finally { m.close(); }
    }
});

await __kitS.section("unsupported kernel name", async () => {
    await throws(() => new SVC({ kernel: "sigmoid" }), "unsupported kernel name", TypeError);
    await throws(() => new SVC({ C: 0 }), "C must be positive", RangeError);
    await throws(() => new SVC({ degree: 0 }), "degree at least 1", RangeError);
    await throws(() => new SVC({ maxIter: 0 }), "maxIter at least 1", RangeError);
    await throws(() => new SVC(7), "options must be an object", TypeError);
    const m = new SVC();
    try {
        await throws(async () => await m.predict([[1]]), "predict before fit");
        assert(m.nSupportVectors === 0, "no support vectors before fit");
        await throws(async () => await m.fit([[1], [2]], [3, 3]), "a single class cannot be separated", RangeError);
        await m.fit([[1], [2]], [0, 1]);
        await throws(async () => await m.predict([[1, 2]]), "wrong feature count", TypeError);
    } finally { m.close(); }
});

await __kitS.section("reentrant close during SVC fit", async () => {
    const m = new SVC({ kernel: "linear" });
    await throws(async () => await m.fit([[{ valueOf() { m.close(); return 1; } }], [2]], [0, 1]),
        "reentrant close during SVC fit");
    assert(m.closed === true, "closed");
});

await __kitS.section("reentrant close during decisionFunction", async () => {
    const m = new SVC({ kernel: "linear", C: 10 });
    await m.fit([[1], [2]], [0, 1]);
    await throws(async () => await m.decisionFunction([[{ valueOf() { m.close(); return 1; } }]]),
        "reentrant close during decisionFunction");
    assert(m.closed === true, "closed");
});

await __kitS.section("wrong error", async () => {
    const X = [[0, 0], [0, 1], [1, 0], [1, 1], [2, 0], [2, 1]];
    const y = [0, 0, 0, 1, 1, 1];
    const throwsRange = async (fn, msg) => {
        n++;
        try { await fn(); } catch (e) {
            if (!(e instanceof RangeError))
                throw new Error(msg + ": wrong error " + e.constructor.name);
            return;
        }
        throw new Error("expected a throw: " + msg);
    };
    await throwsRange(() => new SVC({ degree: 2000 }), "svc degree cap");
    await throwsRange(() => new SVC({ degree: 2 ** 31 }), "svc degree overflow cap");
    await throwsRange(() => new SVC({ maxIter: 100001 }), "svc maxIter cap");
    const m = new SVC({ maxIter: 500 }); await m.fit(X, y);
    await throwsRange(async () => await m.predict([[NaN, 0]]), "svc predict NaN refusal");
    await throwsRange(() => new GaussianMixture(2, { maxIter: 10001 }), "gmm maxIter cap");
    const g = new GaussianMixture(2, { seed: 3 }); await g.fit(X);
    await throwsRange(async () => await g.predict([[NaN, 0]]), "gmm predict NaN refusal");
});

await __kitS.section("svc class-count cap", async () => {
    await throws(async () => {
        const m = new SVC({ maxIter: 100 });
        await m.fit(Array.from({ length: 257 }, (_, i) => [i]),
              Array.from({ length: 257 }, (_, i) => i));
    }, "svc class-count cap", RangeError);
    await throws(async () => {
        const p = new SVC({ kernel: "poly", degree: 100, gamma: 1, coef0: 1 });
        await p.fit([[1e6, 1e6], [2e6, 2e6], [1e6, 2e6], [2e6, 1e6]], [0, 0, 1, 1]);
    }, "svc poly kernel overflow refusal", RangeError);
    const gX = [[0, 0], [0, 1], [1, 0], [1, 1], [2, 0], [2, 1]];
    const g = new GaussianMixture(2, { seed: 3 }); await g.fit(gX);
    await throws(async () => await g.fit([[1.0]]), "gmm refit with rows < k", RangeError);
    assert((await g.predict(gX)).length === gX.length, "failed refit preserves the model");
});
__kitS.end();

print("test_ml_svm: all tests passed (" + n + " assertions)");
