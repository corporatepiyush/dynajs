// timeout: 300
// covers: c:src/builtins/builtin-json.c
// tests/core/test_audit_budget_2.js -- E3-02/03/04 + E3-05: flatten work budget,
// __proto__ group-key pollution, and the uncapped superlinear output forms.
// Runs in-process; no child processes needed.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_json_audit_budget_2", { chain: true });
let failures = 0;

function assert(cond, msg) {
    if (!cond) {
        failures++;
        print("  FAIL:", msg);
    }
}

function throwsRange(fn, name) {
    try {
        fn();
        assert(false, name + ": did not throw");
    } catch (e) {
        assert(e instanceof RangeError,
            name + ": threw " + e + " (wanted RangeError)");
    }
}

// ---- E3-02: flatten work budget ----

__kitS.section("flatten self-containing", () => {
    const a = [];
    a.push(a, a);
    throwsRange(() => a.flatten(), "flatten self-containing");
    throwsRange(() => a.flatten(Infinity), "flatten self-containing depth Inf");
    assert(JSON.stringify([1, [2, [3]]].flatten()) === "[1,2,3]",
        "flatten legit");
    assert(JSON.stringify([1, [2, [3, [4]]]].flatten(2)) === "[1,2,3,[4]]",
        "flatten depth semantics kept (deeper than depth left nested)");
    const deep = [];
    let cur = deep;
    for (let i = 0; i < 600; i++) {
        const next = [];
        next.push(cur, cur);
        cur = next;
    }
    throwsRange(() => cur.flatten(), "flatten deep self-containing");
    assert(JSON.stringify(Array.from({ length: 1000 }, (_, i) => i).flatten(1))
        .length > 0, "flatten large flat array");
    print("  ok  flatten budget");
});
__kitS.end();

if (failures) {
    print("test_audit_budget_2: " + failures + " FAILURES");
    throw new Error("test_audit_budget_2 failed");
}
print("test_audit_budget_2: all tests passed");
