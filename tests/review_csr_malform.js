/* review_csr_malform.js -- e7 review, adversarial CSR construction (P01-P20).
 *
 * Question: can ANY malformed CSR reach a predict kernel? A column index
 * >= cols is an OOB WRITE into coefficient vectors, so every malformed
 * shape must be refused at CONSTRUCTION (the ctor validates) or before a
 * kernel runs. Then sentinel-check the coefficient buffers: fit a model,
 * throw everything at it, prove coef is bit-identical afterwards.
 */
import { CSR, LinearRegression } from "dyna:ml";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
function errOf(fn) {
    try { fn(); return null; } catch (e) { return e; }
}
const cls = (e) => (e ? e.constructor.name + ": " + e.message : "(no error)");

/* Every attack below must throw; the class is pinned too. */
function refused(fn, wantClass, name) {
    const e = errOf(fn);
    ok(e !== null && (wantClass === null || e.constructor.name === wantClass),
       name, e === null ? "ACCEPTED a malformed CSR" : cls(e));
}

/* ---------- malformed column indices (the OOB-write class) ---------- */
P01: {
    refused(() => new CSR([1], [-1], [0, 1], 4), "RangeError",
        "P01 col index -1 refused");
}
P02: {
    refused(() => new CSR([1], [-0.5], [0, 1], 4), "RangeError",
        "P02 col index -0.5 refused");
}
P03: {
    refused(() => new CSR([1], [2147483647], [0, 1], 4), "RangeError",
        "P03 col index 2^31-1 refused (cols=4)");
}
P04: {
    /* beyond uint32: the float->uint32 cast guard must reject, not truncate */
    refused(() => new CSR([1], [4294967296 + 1], [0, 1], 4), "RangeError",
        "P04 col index 2^32+1 refused, no uint32 truncation");
}
P05: {
    refused(() => new CSR([1], [9007199254740993], [0, 1], 4), "RangeError",
        "P05 col index 2^53+1 refused");
}
P06: {
    refused(() => new CSR([1], [0.5], [0, 1], 4), "RangeError",
        "P06 fractional col index refused");
}
P07: {
    refused(() => new CSR([1], [NaN], [0, 1], 4), "RangeError",
        "P07 NaN col index refused");
}
P08: {
    refused(() => new CSR([1], [Infinity], [0, 1], 4), "RangeError",
        "P08 infinite col index refused");
}
P09: {
    refused(() => new CSR([1], [4], [0, 1], 4), "RangeError",
        "P09 col index == cols refused (half-open bound)");
}
P10: {
    refused(() => new CSR([1, 2], [0], [0, 1], 4), "TypeError",
        "P10 columns length != values length refused");
}

/* ---------- malformed rowPointers ---------- */
P11: {
    refused(() => new CSR([1, 2], [0, 1], [0, 2, 1], 4), "RangeError",
        "P11 non-monotonic rowPointers refused");
}
P12: {
    refused(() => new CSR([1, 2], [0, 1], [0, 1], 4), "RangeError",
        "P12 rowPointers tail != nnz refused");
}
P13: {
    refused(() => new CSR([1, 2], [0, 1], [1, 2], 4), "RangeError",
        "P13 rowPointers head != 0 refused");
}
P14: {
    refused(() => new CSR([1, 2], [0, 1], [0, 7], 4), "RangeError",
        "P14 rowPointers entry past nnz refused");
    refused(() => new CSR([1], [0], [0, 0.5], 4), "RangeError",
        "P14b fractional rowPointers refused");
}

/* ---------- values + width validation ---------- */
P15: {
    refused(() => new CSR([NaN], [0], [0, 1], 4), "RangeError",
        "P15 NaN value refused at construction");
    refused(() => new CSR([Infinity], [0], [0, 1], 4), "RangeError",
        "P15b infinite value refused at construction");
}
P16: {
    refused(() => new CSR([], [], [0], 0), "RangeError",
        "P16 cols=0 refused");
    refused(() => new CSR([], [], [0], -3), "RangeError",
        "P16b negative cols refused");
    refused(() => new CSR([1], [0], [], 4), "RangeError",
        "P16c empty rowPointers refused");
}

/* ---------- huge cols, tiny nnz: refuse cleanly, never touch memory ---------- */
P17: {
    const H = new CSR([1.0], [0], [0, 1], Math.pow(2, 40));
    ok(H.cols === Math.pow(2, 40) && H.nnz === 1,
       "P17 huge-cols tiny-nnz CSR constructs (cols is caller-declared)");
    /* against a 3-feature model the width check must refuse BEFORE any
     * scratch (cols doubles) is allocated */
    const m = new LinearRegression();
    m.fit([[1, 0, 0], [0, 1, 0], [0, 0, 1]], [1, 2, 3]);
    const e = errOf(() => m.predict(H));
    ok(e !== null && e.constructor.name === "TypeError" && /features/.test(e.message),
       "P17b predict refuses the wrong width before any kernel/scratch",
       cls(e));
    const e2 = errOf(() => m.fit(H, [1]));   /* the sparse fit: p*p normal matrix */
    ok(e2 !== null, "P17c fit on a 2^40-col CSR throws cleanly (OOM/Range)",
       e2 === null ? "fit ACCEPTED 2^40 features" : cls(e2));
    /* P17d/P17e (row()/toDense() on this handle) SIGSEGV the process -- see
     * tests/review_hugecols.js run under the shell runner. In-process here
     * would kill the suite before its verdict. */
    H.close();
}

/* ---------- valid-but-awkward shapes must be ACCEPTED and consistent ---- */
P18: {
    /* unsorted + duplicate indices + an empty row + a trailing empty row */
    const S = new CSR([2, 3, 5], [3, 3, 0], [0, 3, 3, 3], 4);
    ok(S.rows === 3 && S.nnz === 3, "P18 unsorted+duplicate+empty rows accepted");
    const r = S.row(0);
    ok(r[0] === 5 && r[3] === 5 && Object.is(r[1], 0) && Object.is(r[2], 0),
       "P18b row() reads duplicates as SUM and ignores empty rows (" +
       JSON.stringify(r) + ")");
    S.close();
}
P19: {
    /* all-zero matrix, rows only in rowPointers */
    const Z = new CSR([], [], [0, 0, 0], 4);
    ok(Z.rows === 2 && Z.nnz === 0 && Z.row(1).every((v) => v === 0),
       "P19 zero-nnz CSR with empty rows accepted and reads all-zero");
    Z.close();
}

/* ---------- sentinel: the coefficient buffers survive every attack ------ */
P20: {
    const X = [[1, 0, 0], [0, 1, 0], [0, 0, 1], [1, 1, 1]];
    const y = [3, 5, 7, 15];
    const m = new LinearRegression();
    m.fit(X, y);
    const coefBefore = [...m.coef];
    const intcpBefore = m.intercept;
    /* every malformed shape above, plus hostile argument shapes */
    const attacks = [
        () => new CSR([1], [-1], [0, 1], 4),
        () => new CSR([1], [2147483647], [0, 1], 4),
        () => new CSR([1, 2], [0], [0, 1], 4),
        () => new CSR([1, 2], [0, 1], [0, 2, 1], 4),
        () => new CSR([1], [NaN], [0, 1], 4),
        () => new CSR([NaN], [0], [0, 1], 4),
        () => new CSR([1], [0], [0, 9], 4),
        () => { let g = 0; return new CSR({ length: 2, 0: 1,
                   get 1() { throw new Error("mid-ingest"); } }, [0, 1], [0, 2], 4); },
    ];
    for (const a of attacks) errOf(a);
    /* and wrong-width / closed-handle predicts on VALID csr handles */
    const V = new CSR([1, 1, 1], [0, 1, 2], [0, 3], 3);
    const okPred = m.predict(V);
    for (const a of attacks) errOf(a);
    errOf(() => m.predict(new CSR([1], [9], [0, 1], 3))); /* refused at ctor */
    const C = new CSR([1], [0], [0, 1], 3); C.close();
    errOf(() => m.predict(C));
    errOf(() => m.predict(V, 5, 5));
    const okPred2 = m.predict(V);
    V.close();
    const coefAfter = [...m.coef];
    ok(JSON.stringify(coefBefore) === JSON.stringify(coefAfter) &&
       Object.is(intcpBefore, m.intercept) &&
       Object.is(okPred[0], okPred2[0]) && okPred[0] > 14.9 && okPred[0] < 15.1,
       "P20 coefficient buffers untouched by every refusal (sentinel)",
       "coef " + JSON.stringify(coefBefore) + " -> " + JSON.stringify(coefAfter) +
       " pred " + okPred[0] + "/" + okPred2[0]);
}

print("review_csr_malform: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_csr_malform: " + fail + " failures");
