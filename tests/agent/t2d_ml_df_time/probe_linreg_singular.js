import { LinearRegression, CSR } from "dyna:ml";
function t(l, f) { try { console.log(l, "->", JSON.stringify(f())); } catch (e) { console.log(l, "-> THROWS", e.constructor.name, String(e.message)); } }
t("dup cols fit+predict", () => {
    const m = new LinearRegression().fit([[1, 1], [2, 2], [3, 3]], [1, 2, 3]);
    return { coef: m.coef, intercept: m.intercept, pred: m.predict([[4, 4]]) };
});
t("zero col", () => {
    const m = new LinearRegression().fit([[1, 0], [2, 0], [3, 0]], [2, 4, 6]);
    return { coef: m.coef, intercept: m.intercept, pred: m.predict([[4, 0]]) };
});
t("int collinear", () => {
    const m = new LinearRegression().fit([[1, 2], [2, 4], [3, 6]], [3, 6, 9]);
    return { coef: m.coef, intercept: m.intercept, pred: m.predict([[4, 8]]) };
});
t("empty fit", () => {
    const m = new LinearRegression().fit([], []);
    return { coef: m.coef, intercept: m.intercept, pred: m.predict([]) };
});
t("constant cols", () => {
    const m = new LinearRegression().fit([[1, 5], [1, 5], [1, 5]], [2, 2, 2]);
    return { coef: m.coef, intercept: m.intercept };
});
t("sparse vs dense bit-equal", () => {
    const X = [[1, 0, 2], [0, 3, 0], [4, 0, 5], [1, 1, 0]];
    const y = [7, -2, 11, 4];
    const a = new LinearRegression().fit(X, y);
    const S = CSR.fromDense(X);
    const b = new LinearRegression().fit(S, y);
    return { same: a.coef.every((v, i) => Object.is(v, b.coef[i])) && Object.is(a.intercept, b.intercept), coef: a.coef, int: a.intercept };
});
t("ones weights == no weights", () => {
    const X = [[1], [2], [3], [4]], y = [1, 2, 3, 10];
    const a = new LinearRegression().fit(X, y);
    const b = new LinearRegression().fit(X, y, { sampleWeight: [1, 1, 1, 1] });
    return { same: Object.is(a.intercept, b.intercept) && a.coef.every((v, i) => Object.is(v, b.coef[i])) };
});
