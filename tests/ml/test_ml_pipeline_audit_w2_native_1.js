// timeout: 300
// covers: dyna:ml.Pipeline
// Split from tests/test_audit_w2_native_1.js: the ml sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_pipeline_audit_w2_native_1");
import * as ml from "dyna:ml";
import { args } from "dyna:sys";

const BIN = args()[0];
let failures = 0, checks = 0;
function ok(cond, msg) {
    checks++;
    if (!cond) {
        failures++;
        print("  FAIL:", msg);
    }
}
function eq(got, want, msg) {
    ok(Object.is(got, want), msg + " (got " + String(got) + ", want " + String(want) + ")");
}
function errName(fn) {
    try { fn(); return "none"; } catch (e) { return e.name; }
}
async function errNameAsync(fn) {
    try { await fn(); return "none"; } catch (e) { return e.name; }
}
// ---- resource pin (M1b-04) ---------------------------------------------------

await __kitS.section("a stage cannot close the pipeline that is running it", async () => {
    let pipe, got = "?";
    const evil = { fit() { got = errName(() => pipe.close()); return this; }, transform(X) { return X; } };
    const last = { fit() { return this; }, predict() { return [0]; } };
    pipe = new ml.Pipeline([evil, evil, last]);
    const outer = await errNameAsync(() => pipe.fit([[1, 2], [3, 4]], [0, 1]));
    eq(got, "TypeError", "a stage cannot close the pipeline that is running it");
    eq(outer, "none", "the pipeline finished its fit");
    eq(pipe.closed, false, "the pipeline is still open");
    pipe.close();
    eq(pipe.closed, true, "close works once no method is running");
});
__kitS.end();
if (failures) {
    print("test_ml_audit_w2_native_1: " + failures + " of " + checks + " FAILED");
    throw new Error("test_ml_audit_w2_native_1 failed");
}
print("test_ml_audit_w2_native_1: " + checks + " checks passed");
