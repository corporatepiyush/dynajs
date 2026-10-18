// flags: --std
// Split from tests/test_p2_batch2.js: the core sections.
import * as os from "os";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " (got " + JSON.stringify(a) + " want " + JSON.stringify(b) + ")"); }
{
    const t0 = Date.now();
    os.sleep(50);
    const dt = Date.now() - t0;
    assert(dt >= 45 && dt < 200, "os.sleep 50ms actual " + dt + "ms");
}
if (fails) { print("test_core_p2_batch2: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_core_p2_batch2 failed"); }
print("test_core_p2_batch2: " + n + " assertions, 0 failures");
