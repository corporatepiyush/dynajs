// covers: dyna:semver.Range
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_semver_range");
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

__kitS.section(">=1.0.0 !!!oops", () => {
    assert(sv.satisfies("1.2.3", ">=1.0.0 !!!oops") === false,
           "invalid range -> false");
    let msg = null;
    try { new sv.Range(">=1.0.0 !!!oops"); }
    catch (e) { msg = String(e); }
    assert(msg !== null && msg.includes(">=1.0.0 !!!oops"),
           "invalid range quotes the range (got " + msg + ")");
    msg = null;
    try { new sv.Range("not " + "very ".repeat(30) + "good"); }
    catch (e) { msg = String(e); }
    assert(msg !== null && msg.includes("very"),
           "Range() quotes an over-long range too");
    msg = null;
    try { sv.sort(["1.2.3", "1.2.4", "nope"]); }
    catch (e) { msg = String(e); }
    assert(msg !== null && /index 2/.test(msg),
           "invalid version in array names the index (got " + msg + ")");
});

__kitS.section("new Range", () => {
    const T = [
        ["1.2.3", ["1.2.3", "1.2.4"], ["1.2.3"]],
        ["=1.2.3", ["1.2.3", "1.2.4"], ["1.2.3"]],
        ["^1.2.3", ["1.2.2", "1.2.3", "1.9.9", "2.0.0"], ["1.2.3", "1.9.9"]],
        ["^0.2.3", ["0.2.2", "0.2.3", "0.2.9", "0.3.0"], ["0.2.3", "0.2.9"]],
        ["~1.2.3", ["1.2.2", "1.2.3", "1.2.9", "1.3.0"], ["1.2.3", "1.2.9"]],
        ["1.2.x", ["1.2.0", "1.2.9", "1.3.0"], ["1.2.0", "1.2.9"]],
        ["1.2", ["1.2.5", "1.3.0"], ["1.2.5"]],
        ["*", ["0.0.0", "9.9.9"], ["0.0.0", "9.9.9"]],
        ["", ["0.0.0", "9.9.9"], ["0.0.0", "9.9.9"]],
        ["1.2.3 - 2.3.4", ["1.2.2", "1.5.0", "2.3.4", "2.3.5"],
         ["1.5.0", "2.3.4"]],
        [">=1.2.0 <1.3.0", ["1.2.0", "1.2.9", "1.3.0"], ["1.2.0", "1.2.9"]],
        ["^1.0.0 || ^2.0.0", ["1.5.0", "2.5.0", "3.0.0"], ["1.5.0", "2.5.0"]],
        ["^1.0.0-rc.1", ["1.0.0-rc.1", "1.0.0-rc.2", "1.0.0", "1.0.1-rc.1"],
         ["1.0.0-rc.1", "1.0.0-rc.2", "1.0.0"]],
    ];
    for (const [range, versions, expected] of T) {
        const r = new sv.Range(range);
        assert(r instanceof sv.Range, "new Range(" + JSON.stringify(range) + ")");
        assert(r.source === range,
            "Range.source round-trips " + JSON.stringify(range));
        assert(r.setCount >= 1, "Range.setCount is at least 1");
        for (const v of versions) {
            const want = expected.includes(v);
            assert(r.test(v) === want,
                "Range(" + JSON.stringify(range) + ").test(" + v + ") == " + want);
            assert(r.test(v) === sv.satisfies(v, range),
                "Range.test agrees with satisfies for " + v + " in " + range);
        }
        assert(arrEq(r.filter(versions), expected),
            "Range.filter(" + JSON.stringify(range) + ") keeps the satisfying " +
            "elements in order");
    }
    assert(new sv.Range("^1.0.0 || ^2.0.0").setCount === 2, "setCount counts || sets");
    assert(new sv.Range("^1.0.0").setCount === 1, "setCount of one set");
    throws(() => new sv.Range("!!"), "Range rejects an invalid range");
    throws(() => new sv.Range(123), "Range rejects a number (node parity)");
    throws(() => new sv.Range(), "Range requires an argument");
    {
        const r = new sv.Range("^1.2.3-beta.2 || ~3.4.0-rc.1");
        for (let i = 0; i < 50000; i++) { try { sv.parse("junk" + i); } catch (e) {} }
        assert(r.test("1.2.3-beta.2") === true, "UAF probe: same tuple pre in");
        assert(r.test("1.2.3-beta.1") === false, "UAF probe: below beta.2 out");
        assert(r.test("1.2.3-beta.3") === true, "UAF probe: higher pre in");
        assert(r.test("1.2.3-alpha") === false, "UAF probe: other pre out");
        assert(r.test("3.4.0-rc.2") === true, "UAF probe: tilde pre in");
        assert(r.test("3.4.1") === true, "UAF probe: release not pre-gated");
        assert(r.test("3.5.0") === false, "UAF probe: tilde upper out");
        assert(r.test("1.2.4") === true, "UAF probe: caret release still in");
        assert(r.source === "^1.2.3-beta.2 || ~3.4.0-rc.1", "source intact");
    }
    assert(new sv.Range("^1.0.0").test("junk") === false,
           "Range.test answers false for junk");
});
__kitS.end();

print("test_semver: all tests passed (" + n + " assertions)");
