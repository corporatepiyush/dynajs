// covers: dyna:semver.clean
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_semver_clean");
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

__kitS.section("=v1.2.3", () => {
    const T = [
        ["  =v1.2.3   ", "1.2.3"],
        ["v1.2.3", "1.2.3"],
        ["=1.2.3", "1.2.3"],
        ["vv1.2.3", "1.2.3"],
        ["  1.2.3  ", "1.2.3"],
        ["1.2.3", "1.2.3"],
        ["1.2.3-beta.1", "1.2.3-beta.1"],
        ["1.2.3+build", "1.2.3"],
        ["=v1.2.3-rc.1+meta", "1.2.3-rc.1"],
        ["\t1.2.3\n", "1.2.3"],
    ];
    for (const [inp, exp] of T)
        assert(sv.clean(inp) === exp,
            "clean(" + JSON.stringify(inp) + ") => " + sv.clean(inp));
    assert(sv.clean("not a version") === null, "clean garbage null");
    assert(sv.clean("1.2") === null, "clean partial null");
    assert(sv.clean("") === null, "clean empty null");
    assert(sv.clean("1.2.x") === null, "clean x-range null");
    throws(() => sv.clean(123), "clean(number) throws");
});
__kitS.end();

print("test_semver: all tests passed (" + n + " assertions)");
