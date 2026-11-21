// covers: dyna:semver.sort
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_semver_sort");
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

__kitS.section("sort produces the known order:", () => {
    const ordered = [
        "0.0.0", "0.0.1", "0.1.0", "1.0.0-alpha", "1.0.0-alpha.1",
        "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2", "1.0.0-beta.11",
        "1.0.0-rc.1", "1.0.0", "1.0.1", "1.2.0", "2.0.0", "10.0.0",
    ];
    const shuffled = ordered.slice();
    for (let i = shuffled.length - 1; i > 0; i--) {
        const j = ri(i + 1);
        const t = shuffled[i]; shuffled[i] = shuffled[j]; shuffled[j] = t;
    }
    const before = shuffled.slice();
    const out = sv.sort(shuffled);
    assert(arrEq(out, ordered), "sort produces the known order: " +
        JSON.stringify(out));
    assert(out !== shuffled, "sort returns a fresh array");
    assert(arrEq(shuffled, before), "sort left the input untouched");
    assert(arrEq(sv.sort([]), []), "sort empty");
    assert(arrEq(sv.sort(["1.2.3"]), ["1.2.3"]), "sort singleton");
    const eqp = sv.sort(["1.0.0+b", "1.0.0+a", "1.0.0+c"]);
    assert(arrEq(eqp, ["1.0.0+b", "1.0.0+a", "1.0.0+c"]), "sort stable eqp");
    throws(() => sv.sort(["1.2.3", "bad"]), "sort rejects invalid element");
    throws(() => sv.sort(["1.2.3", 123]), "sort rejects non-string element");
    throws(() => sv.sort("nope"), "sort rejects non-array");
});
__kitS.end();

print("test_semver: all tests passed (" + n + " assertions)");
