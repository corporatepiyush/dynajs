// covers: dyna:mathx.idivide
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_idivide_tierb");
import * as m from "dyna:mathx";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function near(a, b, tol, msg) {
    n++;
    const scale = Math.max(1, Math.abs(b));
    if (!(Math.abs(a - b) <= tol * scale))
        throw new Error("assertion failed: " + msg + " (" + a + " vs " + b + ")");
}
function throws(fn, msg) {
    n++;
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    if (caught === null) throw new Error("assertion failed: " + msg + " (expected throw)");
}

__kitS.section("idivide default is fix (toward zero)", () => {
    near(m.idivide(7, 2), 3, 0, "idivide default is fix (toward zero)");
    near(m.idivide(-7, 2), -3, 0, "fix on a negative truncates toward zero");
    near(m.idivide(-7, 2, "floor"), -4, 0, "floor rounds down");
    near(m.idivide(-7, 2, "ceil"), -3, 0, "ceil rounds up");
    near(m.idivide(-7, 2, "round"), -4, 0, "round is round-half-to-even");
    near(m.idivide(7, 2, "fix"), 3, 0, "explicit fix matches the default");
    throws(() => m.idivide(1, 2, "nearest"), "an unknown mode is refused");
    assert(!isFinite(m.idivide(1, 0)), "division by zero is IEEE, not a throw");

    const p3 = m.perms([1, 2, 3]);
    assert(p3.length === 6, "perms(3) has 3! rows");
    assert(JSON.stringify(p3) === JSON.stringify(
        [[3,2,1],[3,1,2],[2,3,1],[2,1,3],[1,3,2],[1,2,3]]),
        "perms is in reverse lexicographic order, as MATLAB emits it");
    assert(JSON.stringify(m.perms([9])) === "[[9]]", "one element is one row");
    assert(m.perms([]).length === 1, "the empty input has one (empty) permutation");
    assert(m.perms([1,2,3,4,5,6,7,8]).length === 40320, "8! rows");
    throws(() => m.perms([1,2,3,4,5,6,7,8,9]), "9 elements is refused");
    throws(() => m.perms("abc"), "a non-array is refused");
    const seen = new Set();
    for (const row of p3) {
        assert(row.length === 3, "each row has n entries");
        seen.add(row.join(","));
    }
    assert(seen.size === 6, "all six rows are distinct");
});
__kitS.end();

print("test_mathx_tierb: all " + n + " assertions passed");
