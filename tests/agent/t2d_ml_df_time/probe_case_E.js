import { LinearRegression } from "dyna:ml";
const X = [[1e-3, 0, 0], [0, 1e-3, 0], [0, 0, 1e-3], [1, 1, 1]];
const cstar = [1, 2, 3];
const y = X.map(r => r[0] * 1 + r[1] * 2 + r[2] * 3 + 1);
console.log("y:", JSON.stringify(y));
const m = new LinearRegression().fit(X, y);
console.log("coef:", JSON.stringify(m.coef), "intercept:", m.intercept);
for (let j = 0; j < 3; j++)
    console.log("coef[" + j + "] err:", Math.abs(m.coef[j] - cstar[j]));
console.log("intercept err:", Math.abs(m.intercept - 1));
