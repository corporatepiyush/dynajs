// covers: dyna:semver.satisfies
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_semver_satisfies");
import * as sv from "dyna:semver";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function throws(fn, msg) {
    let caught = false;
    try { fn(); } catch (e) { caught = true; }
    assert(caught, "should throw: " + msg);
}
function arrEq(a, b) {
    if (!Array.isArray(a) || !Array.isArray(b)) return false;
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) {
        if (Array.isArray(a[i]) || Array.isArray(b[i])) {
            if (!arrEq(a[i], b[i])) return false;
        } else if (a[i] !== b[i]) return false;
    }
    return true;
}
function sign(x) { return x < 0 ? -1 : x > 0 ? 1 : 0; }

let _seed = 0x2545F491 >>> 0;
function rnd() {
    _seed = (Math.imul(_seed, 1664525) + 1013904223) >>> 0;
    return _seed / 4294967296;
}
function ri(m) { return Math.floor(rnd() * m); }

__kitS.section("satisfies bad version", () => {
    const T = [
        ["1.2.3", "1.2.3", true],
        ["1.2.3", "=1.2.3", true],
        ["1.2.4", "1.2.3", false],
        ["1.2.3", ">1.2.2", true],
        ["1.2.3", ">1.2.3", false],
        ["1.2.3", ">=1.2.3", true],
        ["1.2.3", "<1.2.4", true],
        ["1.2.3", "<1.2.3", false],
        ["1.2.3", "<=1.2.3", true],
        ["1.2.3", ">=1.2.0 <1.3.0", true],
        ["1.3.0", ">=1.2.0 <1.3.0", false],
        ["1.2.3", ">1.0.0 <2.0.0", true],
        [" 1.2.3", "1.2.3", true],
        ["1.2.3", "^1.2.3", true],
        ["1.2.4", "^1.2.3", true],
        ["1.9.9", "^1.2.3", true],
        ["2.0.0", "^1.2.3", false],
        ["1.2.2", "^1.2.3", false],
        ["0.2.3", "^0.2.3", true],
        ["0.2.9", "^0.2.3", true],
        ["0.3.0", "^0.2.3", false],
        ["0.2.2", "^0.2.3", false],
        ["0.0.3", "^0.0.3", true],
        ["0.0.4", "^0.0.3", false],
        ["0.0.2", "^0.0.3", false],
        ["1.5.0", "^1.2.x", true],
        ["2.0.0", "^1.2.x", false],
        ["0.0.5", "^0.0.x", true],
        ["0.1.0", "^0.0.x", false],
        ["0.0.9", "^0.0", true],
        ["0.1.0", "^0.0", false],
        ["1.5.0", "^1.x", true],
        ["2.0.0", "^1.x", false],
        ["0.9.0", "^0.x", true],
        ["1.0.0", "^0.x", false],
        ["1.2.3-beta.2", "^1.2.3-beta.2", true],
        ["1.2.3-beta.4", "^1.2.3-beta.2", true],
        ["1.2.3", "^1.2.3-beta.2", true],
        ["1.9.0", "^1.2.3-beta.2", true],
        ["1.2.3-beta.1", "^1.2.3-beta.2", false],
        ["1.2.4-beta.2", "^1.2.3-beta.2", false],
        ["2.0.0", "^1.2.3-beta.2", false],
        ["0.0.3-beta", "^0.0.3-beta", true],
        ["0.0.3", "^0.0.3-beta", true],
        ["0.0.4", "^0.0.3-beta", false],
        ["1.2.3", "~1.2.3", true],
        ["1.2.9", "~1.2.3", true],
        ["1.3.0", "~1.2.3", false],
        ["1.2.2", "~1.2.3", false],
        ["1.2.0", "~1.2", true],
        ["1.2.9", "~1.2", true],
        ["1.3.0", "~1.2", false],
        ["1.0.0", "~1", true],
        ["1.9.9", "~1", true],
        ["2.0.0", "~1", false],
        ["0.2.3", "~0.2.3", true],
        ["0.2.9", "~0.2.3", true],
        ["0.3.0", "~0.2.3", false],
        ["0.2.0", "~0.2", true],
        ["0.3.0", "~0.2", false],
        ["0.5.0", "~0", true],
        ["1.0.0", "~0", false],
        ["1.2.3-beta.4", "~1.2.3-beta.2", true],
        ["1.2.4-beta.2", "~1.2.3-beta.2", false],
        ["1.2.9", "~1.2.3-beta.2", true],
        ["~>1.2.3", "~>1.2.3", false],
        ["1.2.9", "~> 1.2.3", true],
        ["1.5.0", "1.x", true],
        ["2.0.0", "1.x", false],
        ["1.2.5", "1.2.x", true],
        ["1.3.0", "1.2.x", false],
        ["1.5.0", "1", true],
        ["2.0.0", "1", false],
        ["1.2.5", "1.2", true],
        ["1.3.0", "1.2", false],
        ["3.1.4", "*", true],
        ["0.0.0", "*", true],
        ["3.1.4", "", true],
        ["1.2.3", "1.2.*", true],
        ["1.3.0", "1.2.*", false],
        ["9.9.9", "x", true],
        ["1.2.3", "1.2.3 - 2.3.4", true],
        ["2.3.4", "1.2.3 - 2.3.4", true],
        ["1.5.0", "1.2.3 - 2.3.4", true],
        ["1.2.2", "1.2.3 - 2.3.4", false],
        ["2.3.5", "1.2.3 - 2.3.4", false],
        ["1.2.0", "1.2 - 2.3.4", true],
        ["2.3.4", "1.2 - 2.3.4", true],
        ["1.1.9", "1.2 - 2.3.4", false],
        ["2.3.9", "1.2.3 - 2.3", true],
        ["2.4.0", "1.2.3 - 2.3", false],
        ["2.9.9", "1.2.3 - 2", true],
        ["3.0.0", "1.2.3 - 2", false],
        ["1.5.0", "^1.0.0 || ^2.0.0", true],
        ["2.5.0", "^1.0.0 || ^2.0.0", true],
        ["3.0.0", "^1.0.0 || ^2.0.0", false],
        ["1.0.0", "<1.0.0 || >=2.0.0", false],
        ["2.0.0", "<1.0.0 || >=2.0.0", true],
        ["0.9.0", "<1.0.0 || >=2.0.0", true],
        ["1.2.3-alpha.7", ">1.2.3-alpha.3", true],
        ["3.4.5-alpha.9", ">1.2.3-alpha.3", false],
        ["3.4.5", ">1.2.3-alpha.3", true],
        ["1.2.3-alpha", "*", false],
        ["1.2.3-alpha", ">=1.0.0", false],
        ["1.2.3-alpha", "1.2.3-alpha", true],
        ["1.2.4-alpha", "^1.2.3", false],
        ["1.0.0-rc.1", "^1.0.0-rc.1", true],
        ["1.0.0-rc.2", "^1.0.0-rc.1", true],
        ["1.0.0", "^1.0.0-rc.1", true],
        ["1.0.1-rc.1", "^1.0.0-rc.1", false],
        ["1.5.0", ">=1.2.0 <=1.9.0", true],
        ["1.9.1", ">=1.2.0 <=1.9.0", false],
        ["1.2.3", ">1.2.0 <1.2.3", false],
        ["1.5.0", ">= 1.0.0 < 2.0.0", true],
        ["2.5.0", ">= 1.0.0 < 2.0.0", false],
    ];
    for (const [ver, range, exp] of T) {
        if (ver === "~>1.2.3") {
            throws(() => sv.satisfies(ver, range),
                "satisfies bad version " + JSON.stringify(ver));
            continue;
        }
        const got = sv.satisfies(ver, range);
        assert(got === exp, "satisfies(" + JSON.stringify(ver) + ", " +
            JSON.stringify(range) + ") == " + exp + " (got " + got + ")");
    }
    assert(sv.satisfies("1.2.3", "not a range !!") === false, "bad range false");
    assert(sv.satisfies("1.2.3", ">=x.y.z") === false, "bad range 2 false");
    assert(sv.satisfies("1.2.3", "==1.2.3") === false, "bad operator false");
    assert(sv.satisfies("1.2.3", ">=1.2.3<2.0.0") === false, "glued comps false");
    assert(sv.maxSatisfying([ "1.2.3" ], "not a range !!") === null,
        "maxSatisfying bad range -> null");
    assert(sv.minSatisfying([ "1.2.3" ], "not a range !!") === null,
        "minSatisfying bad range -> null");
    throws(() => sv.satisfies("bogus", ">=1.0.0"), "bad version still throws");
    throws(() => sv.satisfies("1.2.3"), "satisfies missing range");
    assert(sv.maxSatisfying(["bogus", "1.2.3"], "^1.0.0") === "1.2.3",
        "maxSat skips junk element");
    assert(sv.minSatisfying(["bogus"], "*") === null, "minSat junk -> null");
    assert(new sv.Range("*").filter(["bogus", "1.2.3"]).length === 1,
        "filter skips junk element");
    throws(() => sv.sort(["bogus", "1.2.3"]), "sort still rejects junk");
});
__kitS.section("desugared >=0.0.0 is ANY (node-semver replaceGTE0)", () => {
    const T = [
        ["0.0.0-0", "* <0.0.0-2", true], ["0.0.0-0", ">=0.0.0 <0.0.0-2", true],
        ["0.0.0-1", "~x <=0.0.0-1", true], ["0.0.0-beta", "^* ~0.0.0-beta", true],
        ["0.0.0-0", "0.0.0 - 0.0.0-1", true], ["0.0.0-0", "*", false],
        ["0.0.0-0", ">=0.0.0", false], ["0.0.0-3", "* <0.0.0-2", false],
        ["0.0.1-0", "* <0.0.0-2", false],
    ];
    for (const [ver, range, exp] of T) {
        assert(sv.satisfies(ver, range) === exp, "satisfies(" + JSON.stringify(ver) + ", " + JSON.stringify(range) + ") == " + exp);
        assert(new sv.Range(range).test(ver) === exp, "Range(" + JSON.stringify(range) + ").test(" + JSON.stringify(ver) + ") == " + exp);
    }
});
__kitS.end();

print("test_semver: all tests passed (" + n + " assertions)");
