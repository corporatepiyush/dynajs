// covers: c:src/libregexp.c
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_a4_lookahead_budget");
import { hostScale } from "../lib/hostscale.js";
import { cpuNow } from "../lib/cpuclock.js";
let n = 0, fails = 0;
function assert(c, msg) {
    n++;
    if (!c) {
        fails++;
        print("FAIL: " + msg);
    }
}

__kitS.section("nested-group source failed to compile:", () => {
    const depth = 250;
    const body = "a,".repeat(500000) + "a";
    const src = "(".repeat(depth) + body + ")".repeat(depth);
    const t0 = cpuNow();
    let ok = true;
    try {
        new Function(src);
    } catch (e) {
        ok = false;
        print("FAIL: nested-group source failed to compile: " + e.message);
    }
    const ms = cpuNow() - t0;
    assert(ok, "250-deep group with a 1 MB body compiles");
    const cap = Math.round(2000 * hostScale());
    assert(ms < cap, "lookahead work is bounded (took " + ms + " ms, cap " + cap + ")");
});

__kitS.section("x =", () => {
    const src = "x = " + "(".repeat(100) + "1" + ")".repeat(100) + ";";
    const f = new Function(src + " return 1;");
    assert(typeof f === "function", "deep but small nesting still compiles");
});
__kitS.end();

print("test_a4_lookahead_budget: " + (n - fails) + " passed, " + fails + " failed");
if (fails)
    throw new Error("test_a4_lookahead_budget: " + fails + " failures");
