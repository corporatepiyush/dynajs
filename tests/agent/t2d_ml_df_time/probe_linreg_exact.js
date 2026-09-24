/* probe_linreg_exact.js -- forced-optimum systems where the OLS answer is
 * exact by construction (the design is square and invertible, y = X c*). */
import { LinearRegression } from "dyna:ml";

function run(name, X, cstar) {
    const y = X.map(r => {
        let s = 0;
        for (let j = 0; j < r.length; j++) s += r[j] * cstar[j];
        return s + 1; // intercept == 1
    });
    const m = new LinearRegression().fit(X, y);
    const got = m.coef.concat([m.intercept]);
    let maxrel = 0;
    for (let j = 0; j < got.length; j++) {
        const want = cstar[j];
        const err = Math.abs(got[j] - want);
        const rel = err / Math.max(1, Math.abs(want));
        if (rel > maxrel) maxrel = rel;
    }
    console.log(name, "coef+intercept:", JSON.stringify(got),
                "want:", JSON.stringify(cstar.concat([1])),
                "maxrel:", maxrel.toExponential(3));
}

/* 4 rows x 3 features + intercept = 4x4 design, well conditioned */
run("A", [[1, 0, 0], [0, 1, 0], [0, 0, 1], [1, 1, 1]], [2, -1, 3]);
run("B", [[1, 2, 3], [4, 5, 6], [7, 8, 10], [1, 1, 1]], [1, -2, 0.5]);
run("C", [[2, 0, 0], [0, 3, 0], [0, 0, 4], [1, 1, 1]], [1000, -1000, 0.5]);
run("D", [[1e3, 0, 0], [0, 1e3, 0], [0, 0, 1e3], [1, 1, 1]], [1, 2, 3]);
run("E", [[1e-3, 0, 0], [0, 1e-3, 0], [0, 0, 1e-3], [1, 1, 1]], [1, 2, 3]);
