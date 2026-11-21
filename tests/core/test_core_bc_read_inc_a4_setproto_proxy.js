// covers: c:src/serialize/bc_read.inc.c
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_bc_read_inc_a4_setproto_proxy");
let n = 0, fails = 0;
function assert(c, msg) {
    n++;
    if (!c) {
        fails++;
        print("FAIL: " + msg);
    }
}
function errOf(fn) {
    try { fn(); return null; } catch (e) { return e; }
}

__kitS.section("a cycle through a proxy is accepted without calling the trap", () => {
    // OrdinarySetPrototypeOf (step 8.c.i) stops its cycle walk at an object whose [[GetPrototypeOf]] is not the
    // ordinary one, so a cycle through a proxy is accepted and the trap is never called (test262 pins this). The
    // defence that matters is that every later walk over the cycle terminates instead of spinning.
    const base = {};
    let trapCalls = 0;
    const tricky = new Proxy(base, { getPrototypeOf: (t) => (trapCalls++, t) });
    const e = errOf(() => Object.setPrototypeOf(base, tricky));
    assert(e === null && trapCalls === 0, "a cycle through a proxy is accepted without calling the trap (got " + e + ", " + trapCalls + " calls)");
    assert(Object.getPrototypeOf(base) === tricky, "the prototype is the proxy");
    const lookup = errOf(() => "x" in base);
    assert(lookup instanceof RangeError, "a lookup over the proxy cycle ends in a RangeError (got " + lookup + ")");
});

__kitS.section("ordinary 2-cycle is still refused", () => {
    const a = {}, b = {};
    Object.setPrototypeOf(b, a);
    const e = errOf(() => Object.setPrototypeOf(a, b));
    assert(e instanceof TypeError, "ordinary 2-cycle is still refused (got " + e + ")");
});
__kitS.end();

print("test_a4_setproto_proxy: " + (n - fails) + " passed, " + fails + " failed");
if (fails)
    throw new Error("test_a4_setproto_proxy: " + fails + " failures");
