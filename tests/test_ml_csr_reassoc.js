// timeout: 900
import { CSR, LogisticRegression, LinearRegression } from "dyna:ml";

function assert(cond, msg) { if (!cond) throw new Error("FAIL: " + msg); }
function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }

function sparse(rows, cols, k, seed) {
    const rnd = lcg(seed);
    const val = [], col = [], ptr = [0], y = [];
    const seen = new Set();
    for (let i = 0; i < rows; i++) {
        const lab = i % 2;
        seen.clear();
        seen.add(0);
        col.push(0); val.push(lab ? 3.0 : -3.0);
        for (let t = 1; t < k; t++) {
            let c;
            do { c = 1 + Math.floor(rnd() * (cols - 1)); } while (seen.has(c));
            seen.add(c);
            val.push((rnd() * 2 - 1) * 0.2);
            col.push(c);
        }
        seen.clear();
        ptr.push(val.length);
        y.push(lab);
    }
    return { S: new CSR(val, col, ptr, cols), y };
}

const SHAPES = [
    ["k=1 (unroll never runs)", 400, 32, 1, 30],
    ["k=2 (unroll never runs)", 400, 32, 2, 30],
    ["k=3 (unroll never runs)", 400, 64, 3, 30],
    ["k=4 (no tail)", 400, 64, 4, 30],
    ["k=5 (one tail element)", 400, 64, 5, 30],
    ["k=7 (three tail elements)", 400, 128, 7, 30],
    ["k=8 (no tail)", 400, 128, 8, 30],
    ["k=40 (wide row, 200 cols)", 400, 200, 40, 40],
    ["k=40 (narrow row, 64 cols)", 600, 64, 40, 40],
    ["k=150 (dense-ish, 200 cols)", 300, 200, 150, 25],
];
for (const [label, rows, cols, k, iters] of SHAPES) {
    const { S, y } = sparse(rows, cols, k, 4242);
    const m = new LogisticRegression({ maxIter: iters, tol: 1e-9, learningRate: 0.5 });
    m.fit(S, y);
    const p = m.predict(S);
    assert(p.length === rows, `${label}: one label per row`);
    for (let i = 0; i < rows; i++)
        assert(p[i] === y[i], `${label}: separable sparse data is fitted exactly at row ${i}`);
    m.close();
}

{
    const { S, y } = sparse(500, 200, 40, 99);
    const a = new LogisticRegression({ maxIter: 40, tol: 1e-9, learningRate: 0.5 }).fit(S, y);
    const b = new LogisticRegression({ maxIter: 40, tol: 1e-9, learningRate: 0.5 }).fit(S, y);
    const pa = a.predict(S).join(""), pb = b.predict(S).join("");
    assert(pa === pb, "two identical sparse fits give identical labels");
    a.close(); b.close();
}

{
    const rows = 300, cols = 120, k = 30;
    const { S, y } = sparse(rows, cols, k, 7);
    const dense = S.toDense();
    const sp = new LogisticRegression({ maxIter: 30, tol: 1e-9, learningRate: 0.5 }).fit(S, y);
    const dn = new LogisticRegression({ maxIter: 30, tol: 1e-9, learningRate: 0.5 }).fit(dense, y);
    const ps = sp.predict(S).join(""), pd = dn.predict(dense).join("");
    assert(ps === pd, `sparse and dense fits of the same matrix agree label for label`);
    const cs = sp.coef.flat(), cd = dn.coef.flat();
    let worst = 0;
    for (let i = 0; i < cs.length; i++) {
        const rel = Math.abs(cs[i] - cd[i]) / Math.max(1e-12, Math.abs(cd[i]));
        if (rel > worst) worst = rel;
    }
    assert(worst < 1e-6, `sparse/dense coefficients within 1e-6 relative (worst ${worst.toExponential(2)})`);
    sp.close(); dn.close();
}

{
    const rows = 200, cols = 80, k = 20;
    const { S, y } = sparse(rows, cols, k, 11);
    const sp = new LinearRegression().fit(S, y);
    const dn = new LinearRegression().fit(S.toDense(), y);
    const a = sp.coef, b = dn.coef;
    assert(a.length === b.length, "same coefficient count");
    for (let i = 0; i < a.length; i++) {
        assert(Object.is(a[i], b[i]),
               `LinearRegression sparse coef[${i}] is BIT-equal to dense: ${a[i]} vs ${b[i]}`);
    }
    sp.close(); dn.close();
}

{
    const val = [1.5, 2.5, -0.5, 3.0, 1.0, 1.0];
    const col = [3, 3, 3, 0, 1, 1];
    const ptr = [0, 3, 6, 6, 6];
    const cols = 5;
    const S = new CSR(val, col, ptr, cols);
    const rows = 4;
    const dense = S.toDense();
    const r0 = S.row(0), r1 = S.row(1);
    assert(r0[3] === 3.5, `duplicate column 3 sums to 3.5, got ${r0[3]}`);
    assert(r0[1] === 0, `row 0 has nothing at column 1, got ${r0[1]}`);
    assert(r1[0] === 3.0, `row 1 column 0 is 3.0, got ${r1[0]}`);
    assert(r1[1] === 2.0, `duplicate column 1 sums to 2.0, got ${r1[1]}`);
    for (let i = 0; i < rows; i++)
        for (let j = 0; j < cols; j++)
            assert(Object.is(dense[i][j], S.row(i)[j]),
                   `toDense()[${i}][${j}] === row(${i})[${j}]`);
    const y = [1, 0, 1, 0];
    const sp = new LogisticRegression({ maxIter: 20, tol: 1e-9 }).fit(S, y);
    const dn = new LogisticRegression({ maxIter: 20, tol: 1e-9 }).fit(dense, y);
    assert(sp.predict(S).join("") === dn.predict(dense).join(""),
           "duplicate-column rows: sparse and dense agree label for label");
    sp.close(); dn.close();
}

{
    const rows = 500, cols = 1000;
    const val = [], col = [], ptr = [0], y = [];
    const rnd = lcg(5);
    for (let i = 0; i < rows; i++) {
        const c = Math.floor(rnd() * cols);
        col.push(c); val.push(rnd() * 2 - 1); ptr.push(val.length);
        y.push(i % 2);
    }
    const S = new CSR(val, col, ptr, cols);
    const m = new LogisticRegression({ maxIter: 30, tol: 1e-9 }).fit(S, y);
    const p = m.predict(S);
    assert(p.length === rows, "one-nonzero rows: one label per row");
    m.close();
}

{
    const rows = 20, cols = 6;
    const S = new CSR([], [], new Array(rows + 1).fill(0), cols);
    const y = [];
    for (let i = 0; i < rows; i++) y.push(i % 2);
    const m = new LogisticRegression({ maxIter: 50, tol: 1e-12 }).fit(S, y);
    for (const v of m.coef.flat()) assert(v === 0, `zero CSR coefficient is exactly 0, got ${v}`);
    const p = m.predict(S);
    for (const v of p) assert(v === 0 || v === 1, `zero CSR predicts a class, got ${v}`);
    m.close();
}

print("test_ml_csr_reassoc: CSR 4-accumulator dot moves coefficients by ULPs and nothing else (7 groups)");
