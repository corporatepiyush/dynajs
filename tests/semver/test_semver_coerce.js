// covers: dyna:semver.coerce
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_semver_coerce");
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

__kitS.section("foo bar 3.4.5 baz", () => {
    const T = [
        ["1.2.3", "1.2.3"],
        ["v1.2.3", "1.2.3"],
        ["=v1.2.3", "1.2.3"],
        ["v2.3.4-beta.1", "2.3.4"],
        ["1.2.3.4", "1.2.3"],
        ["42.6.7.9.3-alpha", "42.6.7"],
        ["v2", "2.0.0"],
        ["3.5", "3.5.0"],
        ["1", "1.0.0"],
        ["foo bar 3.4.5 baz", "3.4.5"],
        ["version 1.2.3 final", "1.2.3"],
        ["release-10.20.30-rc", "10.20.30"],
        ["  1.2.3  ", "1.2.3"],
        ["a.b.1.2", "1.2.0"],
    ];
    for (const [inp, exp] of T)
        assert(sv.coerce(inp) === exp,
            "coerce(" + JSON.stringify(inp) + ") == " + exp +
            " (got " + sv.coerce(inp) + ")");
    const nulls = ["", "version one", "no digits here", "...", "a.b.c"];
    for (const s of nulls)
        assert(sv.coerce(s) === null, "coerce null " + JSON.stringify(s));
    throws(() => sv.coerce(123), "coerce(number) throws");
    for (const [inp] of T)
        assert(sv.isValid(sv.coerce(inp)), "coerce output valid " + inp);
});
__kitS.end();

print("test_semver: all tests passed (" + n + " assertions)");
