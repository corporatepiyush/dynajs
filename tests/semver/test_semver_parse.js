// covers: dyna:semver.parse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_semver_parse");
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

__kitS.section("parse major", () => {
    const T = [
        ["0.0.0", 0, 0, 0, [], [], "0.0.0"],
        ["1.2.3", 1, 2, 3, [], [], "1.2.3"],
        ["10.20.30", 10, 20, 30, [], [], "10.20.30"],
        ["1.0.0-alpha", 1, 0, 0, ["alpha"], [], "1.0.0-alpha"],
        ["1.0.0-alpha.1", 1, 0, 0, ["alpha", 1], [], "1.0.0-alpha.1"],
        ["1.0.0-0.3.7", 1, 0, 0, [0, 3, 7], [], "1.0.0-0.3.7"],
        ["1.0.0-x.7.z.92", 1, 0, 0, ["x", 7, "z", 92], [], "1.0.0-x.7.z.92"],
        ["1.0.0-x-y-z.--", 1, 0, 0, ["x-y-z", "--"], [], "1.0.0-x-y-z.--"],
        ["1.2.3-beta.11", 1, 2, 3, ["beta", 11], [], "1.2.3-beta.11"],
        ["1.0.0+20130313144700", 1, 0, 0, [], ["20130313144700"], "1.0.0"],
        ["1.0.0-beta+exp.sha.5114f85", 1, 0, 0, ["beta"],
         ["exp", "sha", "5114f85"], "1.0.0-beta"],
        ["1.0.0+21AF26D3---117B344092BD", 1, 0, 0, [],
         ["21AF26D3---117B344092BD"], "1.0.0"],
        ["1.0.0-rc.1+build.123", 1, 0, 0, ["rc", 1], ["build", "123"],
         "1.0.0-rc.1"],
        ["1.2.3+001", 1, 2, 3, [], ["001"], "1.2.3"],
        ["1.2.3-0a", 1, 2, 3, ["0a"], [], "1.2.3-0a"],
        ["99999999999999.1.2", 99999999999999, 1, 2, [], [], "99999999999999.1.2"],
        ["0.0.0-0", 0, 0, 0, [0], [], "0.0.0-0"],
    ];
    for (const [inp, ma, mi, pa, pre, build, ver] of T) {
        const p = sv.parse(inp);
        assert(p.major === ma, "parse major " + inp + " => " + p.major);
        assert(p.minor === mi, "parse minor " + inp);
        assert(p.patch === pa, "parse patch " + inp);
        assert(arrEq(p.prerelease, pre),
            "parse prerelease " + inp + " => " + JSON.stringify(p.prerelease));
        assert(arrEq(p.build, build),
            "parse build " + inp + " => " + JSON.stringify(p.build));
        assert(p.version === ver, "parse version " + inp + " => " + p.version);
        assert(sv.isValid(inp) === true, "isValid true " + inp);
        assert(sv.major(inp) === ma, "major() " + inp);
        assert(sv.minor(inp) === mi, "minor() " + inp);
        assert(sv.patch(inp) === pa, "patch() " + inp);
        if (pre.length === 0)
            assert(sv.prerelease(inp) === null, "prerelease() null " + inp);
        else
            assert(arrEq(sv.prerelease(inp), pre), "prerelease() " + inp);
        for (const id of p.prerelease) {
            assert(typeof id === "number" || typeof id === "string",
                "prerelease id type " + inp);
        }
        for (const id of p.build)
            assert(typeof id === "string", "build id is string " + inp);
    }
});

__kitS.section("1.2.3+bu ild", () => {
    const bad = [
        "", " ", "1", "1.2", "1.2.3.4", "1.2.x", "1.2.*",
        "=1.2.3",
        "01.2.3", "1.02.3", "1.2.03",
        "1.2.3-01",
        "1.2.3-beta.01",
        "1.2.3-", "1.2.3+", "1.2.3-+build",
        "1.2.3-beta..1", "1.2.3-.beta",
        "1.2.3-be!ta", "1.2.3+bu ild", "1.2.3_beta",
        "1.2.-3", "1.-2.3", "-1.2.3", "+1.2.3",
        "1.2.3.", ".1.2.3", "1..2.3",
        "a.b.c", "1.2.beta", "1.2.3beta",
        "1.2.3-beta+", "1.2.3++b",
        "0xff.0.0", "1e3.0.0",
        "99999999999999999999.0.0",
        "9007199254740992.0.0",
        "1.2.3-99999999999999999999",
        "1.2.3-9007199254740992",
    ];
    for (const s of bad) {
        throws(() => sv.parse(s), "parse rejects " + JSON.stringify(s));
        assert(sv.isValid(s) === false, "isValid false " + JSON.stringify(s));
        throws(() => sv.major(s), "major() rejects " + JSON.stringify(s));
    }
    const padded = [" 1.2.3 ", "\t1.2.3", "1.2.3\n", "\f1.2.3",
        "\u00a0v1.2.3", "1.2.3\u3000", "  v0.0.0  ", " 1.0.0-alpha.1+b "];
    for (const s of padded) {
        assert(sv.isValid(s) === true, "isValid trims " + JSON.stringify(s));
        assert(sv.major(s) === sv.major(sv.clean(s)),
            "major on trimmed " + JSON.stringify(s));
    }
    assert(sv.isValid("1.2.3+99999999999999999999") === true,
        "huge numeric BUILD id is valid (opaque)");
    throws(() => sv.parse("1.2.3-99999999999999999999"),
        "huge numeric PRERELEASE id still rejected (MAX_SAFE cap)");
    assert(sv.prerelease("1.0.0-9007199254740991")[0] === "9007199254740991",
        "MAX_SAFE prerelease id stays a string");
    assert(sv.prerelease("1.0.0-9007199254740990")[0] === 9007199254740990,
        "below MAX_SAFE prerelease id is a number");
    assert(JSON.stringify(sv.parse("1.0.0-9007199254740991").prerelease) ===
        '["9007199254740991"]', "parse().prerelease MAX_SAFE string");
    throws(() => sv.parse(), "parse() no args");
    throws(() => sv.parse(123), "parse(number)");
    throws(() => sv.parse(null), "parse(null)");
    assert(sv.isValid(123) === false, "isValid(number) false");
    assert(sv.isValid(null) === false, "isValid(null) false");
    assert(sv.isValid("9007199254740991.0.0") === true, "MAX_SAFE valid");
});
__kitS.end();

print("test_semver: all tests passed (" + n + " assertions)");
