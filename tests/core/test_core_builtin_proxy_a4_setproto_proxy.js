// covers: c:src/builtins/builtin-proxy.c
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_proxy_a4_setproto_proxy");
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

__kitS.section("ordinary proxy prototype set works", () => {
    const target = {};
    const proxy = new Proxy(target, {});
    const o = {};
    Object.setPrototypeOf(o, proxy);
    assert(Object.getPrototypeOf(o) === proxy, "ordinary proxy prototype set works");
});

__kitS.section("proxy over an unrelated target is accepted", () => {
    const base = {};
    const outer = new Proxy(base, {});
    const e = errOf(() => Object.setPrototypeOf(base, outer));
    assert(e === null, "proxy over an unrelated target is accepted (got " + e + ")");
});
__kitS.end();

print("test_a4_setproto_proxy: " + (n - fails) + " passed, " + fails + " failed");
if (fails)
    throw new Error("test_a4_setproto_proxy: " + fails + " failures");
