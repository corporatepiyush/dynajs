// covers: dyna:semver.maxSatisfying
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_semver_max_satisfying");
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

__kitS.section("maxSat ^1.0.0", () => {
    const versions = ["1.2.0", "1.2.3", "1.3.0", "1.9.9", "2.0.0", "2.1.0",
                      "0.9.0"];
    assert(sv.maxSatisfying(versions, "^1.0.0") === "1.9.9", "maxSat ^1.0.0");
    assert(sv.minSatisfying(versions, "^1.0.0") === "1.2.0", "minSat ^1.0.0");
    assert(sv.maxSatisfying(versions, "~1.2.0") === "1.2.3", "maxSat ~1.2.0");
    assert(sv.minSatisfying(versions, "~1.2.0") === "1.2.0", "minSat ~1.2.0");
    assert(sv.maxSatisfying(versions, ">=2.0.0") === "2.1.0", "maxSat >=2.0.0");
    assert(sv.minSatisfying(versions, ">=2.0.0") === "2.0.0", "minSat >=2.0.0");
    assert(sv.maxSatisfying(versions, "^5.0.0") === null, "maxSat none null");
    assert(sv.minSatisfying(versions, "^5.0.0") === null, "minSat none null");
    assert(sv.maxSatisfying([], "*") === null, "maxSat empty null");
    assert(sv.maxSatisfying(versions, "*") === "2.1.0", "maxSat * is highest");
    assert(sv.minSatisfying(versions, "*") === "0.9.0", "minSat * is lowest");
    const pre = ["1.0.0-alpha", "1.0.0-beta", "1.0.0"];
    assert(sv.maxSatisfying(pre, "^1.0.0-alpha") === "1.0.0", "maxSat pre");
    assert(sv.minSatisfying(pre, "^1.0.0-alpha") === "1.0.0-alpha", "minSat pre");
    assert(sv.maxSatisfying(["bad"], "*") === null, "maxSat skips invalid");
    assert(sv.maxSatisfying(versions, "!!!") === null, "maxSat bad range null");
    throws(() => sv.maxSatisfying("nope", "*"), "maxSat rejects non-array");
});
__kitS.end();

print("test_semver: all tests passed (" + n + " assertions)");
