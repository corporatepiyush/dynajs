// covers: dyna:ml.DecisionTreeRegressor
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_decision_tree_regressor_sklearn");
import * as ml from "dyna:ml";
import { DS, E } from "./sklearn_vectors.js";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(got, want, m) {
    n++;
    if (got !== want) throw new Error("assertion failed: " + m +
        "\n  got:  " + got + "\n  want: " + want);
}
function near(got, want, tol, m) {
    n++;
    if (!(Math.abs(got - want) <= tol))
        throw new Error("assertion failed: " + m +
            "\n  got:  " + got + "\n  want: " + want + "\n  tol:   " + tol);
}
function maxdiff(a, b) {
    let d = 0;
    for (let i = 0; i < a.length; i++) d = Math.max(d, Math.abs(a[i] - b[i]));
    return d;
}
const F = (arr) => new Float64Array(arr);
const rowsOf = (flat, cols) => Array.from({ length: flat.length / cols },
    (_, i) => Array.from(flat.slice(i * cols, (i + 1) * cols)));
const cell = (m, i, k, cols) => Array.isArray(m[i]) ? m[i][k] : m[i * cols + k];

const T = (name, cols) => {
    const X = F(DS[name][0]);
    const y = DS[name][1] ? F(DS[name][1]) : null;
    return { X, y, rows: X.length / cols, cols };
};

await __kitS.section("tree regressor predict vs sklearn", async () => {
    {
        const { X, y, rows, cols } = T("tree", 3);
        const m = await new ml.DecisionTreeRegressor({ maxDepth: 4 })
            .fit(X, y, rows, cols);
        assert(maxdiff(await m.predict(X, rows, cols), E.dtr.pred) <= 1e-9,
               "tree regressor predict vs sklearn");
    }
    {
        const { X, y, rows, cols } = T("tc", 3);
        const m = await new ml.DecisionTreeClassifier({ maxDepth: 4 })
            .fit(X, y, rows, cols);
        assert(maxdiff(await m.predict(X, rows, cols), E.dtc.pred) === 0,
               "tree classifier labels vs sklearn");
        {
            const proba = await m.predictProba(X, rows, cols);
            let d = 0;
            for (let i = 0; i < rows; i++)
                for (let k = 0; k < 2; k++)
                    d = Math.max(d, Math.abs(cell(proba, i, k, 2) - E.dtc.proba[i * 2 + k]));
            assert(d <= 1e-9, "tree classifier proba vs sklearn (maxdiff " + d + ")");
        }
    }
});
__kitS.end();

print("test_ml_sklearn: all " + n + " comparisons passed");
