// covers: dyna:semver.lt
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_semver_lt");
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

__kitS.section("gte", () => {
    const chain = [
        "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
        "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0",
    ];
    for (let i = 0; i < chain.length; i++) {
        for (let j = 0; j < chain.length; j++) {
            const a = chain[i], b = chain[j];
            const exp = sign(i - j);
            assert(sv.compare(a, b) === exp,
                "compare(" + a + "," + b + ") == " + exp);
            assert(sv.gt(a, b) === (i > j), "gt " + a + " " + b);
            assert(sv.gte(a, b) === (i >= j), "gte " + a + " " + b);
            assert(sv.lt(a, b) === (i < j), "lt " + a + " " + b);
            assert(sv.lte(a, b) === (i <= j), "lte " + a + " " + b);
            assert(sv.eq(a, b) === (i === j), "eq " + a + " " + b);
            assert(sv.neq(a, b) === (i !== j), "neq " + a + " " + b);
        }
    }
    assert(sv.lt("1.0.0-beta.2", "1.0.0-beta.11"), "beta.2 < beta.11 numeric");
    assert(sv.lt("1.0.0-alpha.9", "1.0.0-alpha.10"), "alpha.9 < alpha.10");
    assert(sv.gt("1.0.0-alpha.10", "1.0.0-alpha.9"), "alpha.10 > alpha.9");
    assert(sv.lt("1.0.0-1", "1.0.0-alpha"), "numeric < alnum");
    assert(sv.lt("1.0.0-alpha.1", "1.0.0-alpha.beta"), "alpha.1 < alpha.beta");
});
__kitS.end();

print("test_semver: all tests passed (" + n + " assertions)");
