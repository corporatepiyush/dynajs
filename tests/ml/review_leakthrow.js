import { CSR, SVC, LogisticRegression, KNClassifier, KMeans, GaussianMixture } from "dyna:ml";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
async function errOf(fn) { try { return { v: await fn() }; } catch (e) { return { e }; } }

P81: {
    const svc = new SVC({ kernel: "poly", degree: 8, gamma: 1, coef0: 1 });
    await svc.fit([[1, 0], [0, 1], [1, 1], [2, 1]], [0, 1, 0, 1]);
    const S = new CSR([1e50, 1e50], [0, 1], [0, 2], 2);
    let threw = 0, msg = "";
    for (let i = 0; i < 200; i++) {
        const e = await errOf(async () => await svc.predict(S));
        if (e.e) { threw++; msg = e.e.message; }
    }
    ok(threw === 200,
       "P81 poly-overflow refusal on a CSR predict x200 (post-scratch throw path)",
       "threw " + threw + "/200: " + msg);
    svc.close(); S.close();
}
P82: {
    const kc = await new KNClassifier(3).fit([[1, 0], [0, 1], [1, 1]], [0, 1, 0]);
    const W = await CSR.fromDense([[1, 2, 3]]);
    const N = new CSR([1e308, 1e308], [0, 0], [0, 2], 2);
    let n = 0;
    for (let i = 0; i < 200; i++) {
        if ((await errOf(async () => await kc.predict(W))).e) n++;
        if ((await errOf(async () => await kc.predict(N))).e) n++;
        if ((await errOf(async () => await CSR.fromDense([[NaN, 1]]))).e) n++;
        const C = await CSR.fromDense([[1, 0]]);
        C.close();
        if ((await errOf(async () => await kc.predict(C))).e) n++;
    }
    ok(n === 800, "P82 pre-scratch refusals x800 clean (" + n + ")");
    kc.close(); W.close(); N.close();
}
P83: {
    const km = await new KMeans(2, 3).fit([[1, 0], [0, 1]]);
    const gmm = await new GaussianMixture(2, { seed: 1 }).fit([[1, 0], [0, 1], [1, 1]]);
    const lg = await new LogisticRegression().fit([[1, 0], [0, 1], [1, 1], [2, 1]], [0, 1, 0, 1]);
    const S = new CSR([1, 2], [0, 1], [0, 2], 2);
    let stable = true;
    for (let i = 0; i < 1000; i++) {
        if ((await km.predict(S))[0] !== (await km.predict(S))[0]) stable = false;
        await gmm.predictProba(S);
        await lg.predict(S);
        if (i % 100 === 0) {
            const junk = new Array(500).fill(i);
            if (junk[1] === -1) stable = false;
        }
    }
    ok(stable, "P83 1000 scratch alloc/free cycles stable, no UAF/leak");
    km.close(); gmm.close(); lg.close(); S.close();
}

print("review_leakthrow: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_leakthrow: " + fail + " failures");
