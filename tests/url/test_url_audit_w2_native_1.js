// flags: --std
// timeout: 300
// Split from tests/test_audit_w2_native_1.js: the url sections.
import { URL, URLSearchParams } from "dyna:url";
import { args } from "dyna:sys";

const BIN = args()[0];
let failures = 0, checks = 0;
function ok(cond, msg) {
    checks++;
    if (!cond) {
        failures++;
        print("  FAIL:", msg);
    }
}
function eq(got, want, msg) {
    ok(Object.is(got, want), msg + " (got " + String(got) + ", want " + String(want) + ")");
}
function errName(fn) {
    try { fn(); return "none"; } catch (e) { return e.name; }
}
async function errNameAsync(fn) {
    try { await fn(); return "none"; } catch (e) { return e.name; }
}
// ---- N2-A / N2-B / N2-C --------------------------------------------------------
{
    const sp = new URLSearchParams("a=1&b=2&c=3");
    let fired = 0;
    Object.defineProperty(Array.prototype, "0", { configurable: true, set(v) { fired++; sp.delete("a"); sp.delete("b"); sp.delete("c"); } });
    let out;
    try {
        const it = sp.entries();
        out = it.next();
    } finally {
        delete Array.prototype[0];
    }
    eq(fired, 0, "URLSearchParams iterator result never reaches an Array.prototype setter");
    eq(out.value.join("="), "a=1", "URLSearchParams iterator yields own-property pairs");
    let vfired = 0;
    Object.defineProperty(Object.prototype, "value", { configurable: true, set(v) { vfired++; } });
    let r2;
    try { r2 = sp.keys().next(); } finally { delete Object.prototype.value; }
    eq(vfired, 0, "iterator result object is built with own properties");
    eq(r2.value, "a", "iterator result is intact");

    const cost = (n) => {
        const u = new URL("http://h/");
        const p = "a/".repeat(n) + "../".repeat(n);
        const t0 = performance.now();
        u.pathname = p;
        return [performance.now() - t0, u.pathname];
    };
    cost(2000);
    const [c1] = cost(20000), [c8, path] = cost(160000);
    eq(path, "/", "dot segments cancel every pushed segment");
    ok(c8 < Math.max(c1, 0.2) * 40, "pathname setter is linear in its input (20k " + c1.toFixed(2) + "ms, 160k " + c8.toFixed(2) + "ms)");
    eq(new URL("http://h/a/b/../c/./d/..").pathname, "/a/c/", "dot-segment semantics (control)");
    eq(new URL("http://h/a/%2e%2e/b").pathname, "/b", "encoded dot segments (control)");
    eq(new URL("file:///C:/a/../../b").pathname, "/C:/b", "file drive letter is not popped (control)");

    const host = (n) => { let s = ""; for (let i = 0; i < n; i++) s += String.fromCodePoint(0x4E00 + (i % 20000)); return s; };
    eq(URL.canParse("http://" + host(4096 - 4 - 1) + ".com/"), true, "IDNA host one under the 4096 code point bound parses");
    eq(URL.canParse("http://" + host(4096 - 4) + ".com/"), true, "IDNA host of exactly 4096 mapped code points parses");
    eq(URL.canParse("http://" + host(4096 - 4 + 1) + ".com/"), false, "IDNA host of 4097 mapped code points is refused");
    eq(URL.canParse("http://" + host(20000) + ".com/"), false, "a far larger IDNA host is refused");
    eq(new URL("http://b\u00fccher.example/").hostname, "xn--bcher-kva.example", "IDNA mapping (control)");
}
if (failures) {
    print("test_url_audit_w2_native_1: " + failures + " of " + checks + " FAILED");
    throw new Error("test_url_audit_w2_native_1 failed");
}
print("test_url_audit_w2_native_1: " + checks + " checks passed");
