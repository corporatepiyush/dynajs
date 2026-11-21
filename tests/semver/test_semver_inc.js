// covers: dyna:semver.inc
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_semver_inc");
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

__kitS.section("(got", () => {
    const T = [
        ["1.2.3", "major", undefined, "2.0.0"],
        ["1.2.3", "minor", undefined, "1.3.0"],
        ["1.2.3", "patch", undefined, "1.2.4"],
        ["1.2.0", "major", undefined, "2.0.0"],
        ["1.0.0", "minor", undefined, "1.1.0"],
        ["1.2.3", "premajor", undefined, "2.0.0-0"],
        ["1.2.3", "preminor", undefined, "1.3.0-0"],
        ["1.2.3", "prepatch", undefined, "1.2.4-0"],
        ["1.2.3", "prerelease", undefined, "1.2.4-0"],
        ["1.0.0", "prerelease", undefined, "1.0.1-0"],
        ["1.2.0-beta.1", "prerelease", undefined, "1.2.0-beta.2"],
        ["1.2.0-beta.0", "prerelease", undefined, "1.2.0-beta.1"],
        ["1.2.0-beta", "prerelease", undefined, "1.2.0-beta.0"],
        ["1.2.0-0", "prerelease", undefined, "1.2.0-1"],
        ["1.0.0-5", "major", undefined, "1.0.0"],
        ["1.1.0-5", "major", undefined, "2.0.0"],
        ["1.2.0-5", "minor", undefined, "1.2.0"],
        ["1.2.1-5", "minor", undefined, "1.3.0"],
        ["1.2.0-5", "patch", undefined, "1.2.0"],
        ["1.2.3-5", "patch", undefined, "1.2.3"],
        ["1.2.3", "premajor", "beta", "2.0.0-beta.0"],
        ["1.2.3", "preminor", "alpha", "1.3.0-alpha.0"],
        ["1.2.3", "prepatch", "rc", "1.2.4-rc.0"],
        ["1.2.3", "prerelease", "beta", "1.2.4-beta.0"],
        ["1.0.0", "prerelease", "beta", "1.0.1-beta.0"],
        ["1.2.0-beta.1", "prerelease", "beta", "1.2.0-beta.2"],
        ["1.2.0-beta.1", "prerelease", "alpha", "1.2.0-alpha.0"],
        ["1.2.0-alpha.0", "prerelease", "alpha", "1.2.0-alpha.1"],
        ["1.2.0-beta", "prerelease", "beta", "1.2.0-beta.0"],
    ];
    for (const [ver, rel, id, exp] of T) {
        const got = id === undefined ? sv.inc(ver, rel) : sv.inc(ver, rel, id);
        assert(got === exp, "inc(" + ver + ", " + rel +
            (id === undefined ? "" : ", " + id) + ") == " + exp +
            " (got " + got + ")");
        assert(sv.isValid(got), "inc result valid: " + got);
    }
    throws(() => sv.inc("1.2.3", "bogus"), "inc bad release");
    throws(() => sv.inc("bad", "major"), "inc bad version");
    throws(() => sv.inc("1.2.3"), "inc missing release");
    throws(() => sv.inc("1.2.3", "major", 5), "inc identifier must be string");

    assert(sv.inc("1.0.0-rc.1", "release") === "1.0.0",
           "inc release drops the prerelease");
    assert(sv.inc("1.2.3-5", "release") === "1.2.3",
           "inc release drops a numeric prerelease");
    assert(sv.inc("1.0.0-alpha.beta", "release") === "1.0.0",
           "inc release drops an alphanumeric prerelease");
    assert(sv.inc("2.0.0-0", "release") === "2.0.0",
           "inc release drops the '0' prerelease");
    assert(sv.inc("1.0.0-rc.1+build.7", "release") === "1.0.0",
           "inc release strips prerelease and build metadata");
    assert(sv.inc("1.0.0", "release") === null,
           "inc release with no prerelease is null (node pins null)");
    assert(sv.inc("1.0.0+build.7", "release") === null,
           "inc release with only build metadata is null (node pins null)");
    assert(sv.inc("1.0.0-rc.1", "release", "beta") === "1.0.0",
           "inc release ignores the identifier (node pins this)");
    throws(() => sv.inc("1.2.3", "releaze"), "inc near-miss release refused");
    throws(() => sv.inc("1.2.3", "Release"), "inc release is case-sensitive");

    throws(() => sv.inc("9007199254740991.0.0", "major"),
           "inc major past MAX_SAFE is refused");
    throws(() => sv.inc("1.9007199254740991.0", "minor"),
           "inc minor past MAX_SAFE is refused");
    throws(() => sv.inc("1.2.3-9007199254740991", "prerelease"),
           "inc numeric prerelease id past MAX_SAFE is refused");
    assert(sv.inc("9007199254740990.0.0", "major") === "9007199254740991.0.0",
           "inc up TO MAX_SAFE still works");
});
__kitS.end();

print("test_semver: all tests passed (" + n + " assertions)");
