import { LinearRegression } from "dyna:ml";
/* on the MUTATED (raw-load) binary: mixed-unit + corners */
for (const M of [1e0, 1e4, 1e6]) {
    try {
        const m = new LinearRegression().fit([[1, M], [2, M], [3, M], [4, M]], [7, 9, 11, 13]);
        const tp = m.predict([[1, M], [2, M], [3, M], [4, M]]);
        print("M=" + M.toExponential(0), "coef0=" + m.coef[0].toExponential(4),
              "trainErr~" + Math.abs(tp[0] - 7).toExponential(3));
        m.close();
    } catch (e) { print("M=" + M.toExponential(0), "THREW: " + e.message); }
}
try {
    const m = new LinearRegression().fit([[1, 1e6], [2, 1e6], [3, 1e6], [4, 1e6]],
        [3, 5, 7, 9], { sampleWeight: [1e300, 1e300, 1e300, 1e300] });
    print("1e300x1e6 ok coef0=" + m.coef[0]);
    m.close();
} catch (e) { print("1e300x1e6 THREW: " + e.message); }
try {
    const m = new LinearRegression().fit([[1, 1], [2, 2], [3, 3]], [1, 2, 3],
        { sampleWeight: [1e-320, 1e-320, 1e-320] });
    print("1e-320 ok coef0=" + m.coef[0]);
    m.close();
} catch (e) { print("1e-320 THREW: " + e.message); }
