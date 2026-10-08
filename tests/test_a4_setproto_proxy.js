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

{
    const base = {};
    const tricky = new Proxy(base, { getPrototypeOf: (t) => t });
    const e = errOf(() => Object.setPrototypeOf(base, tricky));
    assert(e instanceof TypeError, "proxy [[GetPrototypeOf]] cycle is refused (got " + e + ")");
    assert(Object.getPrototypeOf(base) === Object.prototype, "base prototype is unchanged after refusal");
}

{
    const target = {};
    const proxy = new Proxy(target, {});
    const o = {};
    Object.setPrototypeOf(o, proxy);
    assert(Object.getPrototypeOf(o) === proxy, "ordinary proxy prototype set works");
}

{
    const a = {}, b = {};
    Object.setPrototypeOf(b, a);
    const e = errOf(() => Object.setPrototypeOf(a, b));
    assert(e instanceof TypeError, "ordinary 2-cycle is still refused (got " + e + ")");
}

{
    const base = {};
    const outer = new Proxy(base, {});
    const e = errOf(() => Object.setPrototypeOf(base, outer));
    assert(e === null, "proxy over an unrelated target is accepted (got " + e + ")");
}

print("test_a4_setproto_proxy: " + (n - fails) + " passed, " + fails + " failed");
if (fails)
    throw new Error("test_a4_setproto_proxy: " + fails + " failures");
