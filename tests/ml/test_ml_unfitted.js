// covers: contract dyna:ml.* -- reflective unfitted-state probe over every exported class with fit()
// tests/ml/test_ml_unfitted.js -- using a model before fit() is a plain Error
// "<Model>.<op>: call fit() first" (docs/ERRORS.md class tier), the same shape
// for every estimator, transformer and Pipeline. It used to be an
// InternalError "ml.predict: predict before fit" that named neither the model
// nor the remedy; InternalError is reserved for engine faults.
import { sections as __kitSections } from "../lib/kit.js";
import * as ml from "dyna:ml";
const __kitS = __kitSections("test_ml_unfitted", { chain: true });

let n = 0, failures = 0;
function check(cond, msg) { n++; if (!cond) { failures++; print("  FAIL:", msg); } }

async function expectUnfitted(model, cls, op, call) {
    try {
        await call();
        check(false, cls + "." + op + " did not throw");
    } catch (e) {
        check(e.constructor === Error, cls + "." + op + " throws a plain Error, got " + e.constructor.name);
        check(e.message === cls + "." + op + ": call fit() first", cls + "." + op + " message: " + JSON.stringify(e.message));
    }
}

const METHODS = ["predict", "predictProba", "transform", "apply"];
const GETTERS = { LogisticRegression: ["coef", "intercept", "classes"] };
let classes = 0;
await __kitS.section("every unfitted fit/predict class refuses predict and featureImportances", async () => {
for (const cls of Object.keys(ml).sort()) {
    const C = ml[cls];
    if (typeof C !== "function" || !C.prototype || typeof C.prototype.fit !== "function") continue;
    const make = () => cls === "Pipeline" ? new C([new ml.StandardScaler(), new ml.LinearRegression()]) : new C();
    let seen = 0;
    for (const op of METHODS) {
        if (typeof C.prototype[op] !== "function") continue;
        seen++;
        await expectUnfitted(make(), cls, op, () => make()[op]([[1, 2]]));
    }
    if (Object.getOwnPropertyDescriptor(C.prototype, "featureImportances")) {
        seen++;
        await expectUnfitted(make(), cls, "featureImportances", () => make().featureImportances);
    }
    for (const g of GETTERS[cls] || []) {
        seen++;
        await expectUnfitted(make(), cls, g, () => make()[g]);
    }
    if (seen) classes++;
}
});
__kitS.section("every fit/predict class was probed", () => {
check(classes >= 18, "every fit/predict class was probed (" + classes + ")");
});
__kitS.end();
if (!__kitS.listing) {

print("test_ml_unfitted: " + n + " checks over " + classes + " classes, " + failures + " failures");
if (failures) throw new Error("test_ml_unfitted: " + failures + " failures");
}
