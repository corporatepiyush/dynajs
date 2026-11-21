// flags: --std
// covers: c:src/modules/libc/libc-os.c
// Split from tests/test_p2_batch2.js: the core sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_p2_batch2");
import * as os from "os";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " (got " + JSON.stringify(a) + " want " + JSON.stringify(b) + ")"); }
__kitS.section("os.sleep 50ms actual", () => {
    let dt = Infinity;
    for (let i = 0; i < 3 && dt >= 200; i++) {
        const t0 = Date.now();
        os.sleep(50);
        dt = Math.min(dt, Date.now() - t0);
    }
    assert(dt >= 45 && dt < 200, "os.sleep 50ms actual " + dt + "ms (best of up to 3)");
});
__kitS.end();
if (fails) { print("test_core_p2_batch2: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_core_p2_batch2 failed"); }
print("test_core_p2_batch2: " + n + " assertions, 0 failures");
