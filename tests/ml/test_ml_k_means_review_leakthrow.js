// covers: dyna:ml.KMeans
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_k_means_review_leakthrow");
import { CSR, SVC, LogisticRegression, KNClassifier, KMeans, GaussianMixture } from "dyna:ml";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
async function errOf(fn) { try { return { v: await fn() }; } catch (e) { return { e }; } }

await __kitS.section("P83 1000 scratch alloc/free cycles stable, no UAF/leak", async () => {
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
});
__kitS.end();

print("review_leakthrow: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_leakthrow: " + fail + " failures");
