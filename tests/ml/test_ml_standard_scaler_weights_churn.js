// flags: --std
// covers: dyna:ml.StandardScaler
import * as ml from "dyna:ml";
import * as std from "std";
import * as file from "dyna:file";
import { Path } from "dyna:file";
import {
    LinearRegression, LogisticRegression, GaussianNB, StandardScaler,
    PCA, KNRegressor, DecisionTreeRegressor,
} from "dyna:ml";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
async function throwsKind(fn, kind, msg) {
    n++;
    try { await fn(); } catch (e) {
        if (kind && !(e instanceof kind))
            throw new Error(msg + ": wrong error type: " + e);
        return;
    }
    throw new Error(msg + ": expected a throw");
}

const X = [[1], [2], [3], [4], [5], [6]];
const y = [1, 2, 3, 4, 8, 12];
const yc = [0, 0, 0, 1, 1, 1];
const w = [1, 1, 2, 1, 0.5, 2];

for (let i = 0; i < 200; i++) {
    await throwsKind(async () => await new LinearRegression().fit([[1], [2], [NaN]], [1, 2, 3],
                                                { sampleWeight: [1, 1, 1] }),
               RangeError, "NaN fit with weights refuses");
    await throwsKind(async () => await new LogisticRegression().fit([[1], [2], [Infinity]], [0, 1, 1],
                                                  { sampleWeight: [1, 1, 1] }),
               RangeError, "infinite fit with weights refuses");
    await throwsKind(async () => await new StandardScaler().fit([[1], [2]], { sampleWeight: [1, -1] }),
               RangeError, "negative weight refuses");
    await throwsKind(async () => await new StandardScaler().fit([[1], [2]], { sampleWeight: [0, 0] }),
               RangeError, "all-zero weights refuse");
    await throwsKind(async () => await new PCA(1).fit(X, { sampleWeight: w }),
               TypeError, "an unweighted estimator refuses sampleWeight");
    await throwsKind(async () => await new KNRegressor(2).fit(X, y, { sampleWeight: w }),
               TypeError, "KNRegressor refuses sampleWeight");
}

print("test_ml_weights_churn: all " + n + " assertions passed");
