/* review_csr_lifetime.js -- e7 review, CSR lifetime + row-pointer hazards
 * (P21-P26).
 *
 * The C-level contract says a row pointer returned by dyn_ml_row_at is
 * invalidated by the next call. At the JS surface nothing may dangle: every
 * row()/toDense()/predict result must be a fresh copy that survives any
 * later call on the same handle, predictInto outputs must not alias, and
 * repeated predicts on one handle must be stable (scratch reuse).
 */
import { CSR, KMeans, KNClassifier, RandomForestRegressor } from "dyna:ml";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
function errOf(fn) { try { return { v: fn() }; } catch (e) { return { e }; } }

P21: {
    /* hold row results across other calls on the same CSR */
    const S = new CSR([1, 2, 3, 4], [0, 1, 0, 1], [0, 2, 4], 2);
    const r0 = S.row(0), r1 = S.row(1);
    const before = JSON.stringify([r0, r1]);
    const d = S.toDense();
    const m = new KMeans(2, 1).fit([[1, 2], [3, 4]]);
    m.predict(S);
    m.predict(S.toDense());
    S.row(0); S.row(1); S.toDense();
    ok(JSON.stringify([r0, r1]) === before,
       "P21 held row() arrays stay valid and unchanged across later calls",
       before + " -> " + JSON.stringify([r0, r1]));
    /* mutate the held row: the handle must not be affected */
    r0[0] = 999; r0.push(7);
    ok(S.row(0)[0] === 1 && S.row(0).length === 2,
       "P21b mutating a held row() result never touches the CSR");
    S.close();
}
P22: {
    /* hold toDense() across calls */
    const S = new CSR([5], [1], [0, 1, 1], 2);
    const D = S.toDense();
    const before = JSON.stringify(D);
    S.row(0); S.row(1); S.toDense();
    ok(JSON.stringify(D) === before,
       "P22 a held toDense() result is a copy, stable across calls");
    D[0][0] = 12345;
    ok(S.toDense()[0][0] === 0, "P22b mutating toDense() output is harmless");
    S.close();
}
P23: {
    /* repeated predicts on one handle: the row scratch is reused -- results
     * must be identical every time (no stale-scratch bleed between rows) */
    const S = CSR.fromDense([[1, 0, 2], [0, 3, 0], [4, 0, 5], [0, 0, 0]]);
    const kc = new KNClassifier(1).fit([[1, 0, 2], [0, 3, 0], [4, 0, 5], [0, 0, 0]],
                                       [1, 2, 3, 3]);
    const first = JSON.stringify(kc.predict(S));
    let stable = true;
    for (let i = 0; i < 100; i++)
        if (JSON.stringify(kc.predict(S)) !== first) { stable = false; break; }
    ok(stable, "P23 100 predicts on one CSR are bit-stable (scratch reuse)", first);
    kc.close(); S.close();
}
P24: {
    /* predictInto writes the caller's buffer; it must not alias anything the
     * CSR owns: interleave row() calls between two predictIntos */
    const S = new CSR([1, 2], [0, 1], [0, 2], 2);
    const km = new KMeans(2, 7).fit([[0, 0], [9, 9]]);
    const out1 = new Float64Array(1), out2 = new Float64Array(1);
    km.predictInto(out1, S);
    const r = S.row(0);
    km.predictInto(out2, S);
    ok(out1[0] === out2[0] && r[0] === 1 && r[1] === 2,
       "P24 predictInto(CSR) and row() interleave safely (" +
       out1[0] + "," + out2[0] + ")");
    km.close(); S.close();
}
P25: {
    /* out-of-range row() indices */
    const S = new CSR([1], [0], [0, 1], 1);
    const a = errOf(() => S.row(1));
    const b = errOf(() => S.row(-1));
    const c = errOf(() => S.row(2147483647));
    ok(a.e && a.e.constructor.name === "RangeError" &&
       b.e && b.e.constructor.name === "RangeError" &&
       c.e && c.e.constructor.name === "RangeError",
       "P25 row() OOB indices refused (rows, -1, 2^31-1)",
       JSON.stringify([a.e && a.e.message, b.e && b.e.message, c.e && c.e.message]));
    S.close();
}
P26: {
    /* a matrix consumed mid-flight by one predict must be reusable after a
     * GC-ish churn: allocate garbage between calls */
    const S = CSR.fromDense([[2, 0], [0, 2]]);
    const m = new RandomForestRegressor({ nEstimators: 3, seed: 5 });
    m.fit([[2, 0], [0, 2]], [4, 4]);
    let junk = [];
    let stable = true;
    const first = JSON.stringify(m.predict(S));
    for (let i = 0; i < 50; i++) {
        junk.push(new Array(1000).fill(i));
        if (junk.length > 20) junk = [];
        if (JSON.stringify(m.predict(S)) !== first) { stable = false; break; }
    }
    ok(stable, "P26 CSR predict survives alloc churn between calls (no UAF)", first);
    m.close(); S.close();
}

print("review_csr_lifetime: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_csr_lifetime: " + fail + " failures");
