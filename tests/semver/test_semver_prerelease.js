// covers: dyna:semver.prerelease
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_semver_prerelease");
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

__kitS.section("prerelease empty -> null", () => {
    assert(sv.major("2.3.4") === 2, "major");
    assert(sv.minor("2.3.4") === 3, "minor");
    assert(sv.patch("2.3.4") === 4, "patch");
    assert(arrEq(sv.prerelease("1.2.3-beta.11"), ["beta", 11]), "prerelease");
    assert(sv.prerelease("1.2.3") === null, "prerelease empty -> null");
    assert(arrEq(sv.prerelease("1.2.3-0.a.9"), [0, "a", 9]), "prerelease mixed");
    throws(() => sv.prerelease("bad"), "prerelease rejects invalid");
});
__kitS.end();

print("test_semver: all tests passed (" + n + " assertions)");
