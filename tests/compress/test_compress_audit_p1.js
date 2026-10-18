// flags: --std
// Split from tests/test_audit_p1.js: the compress sections.
import { Compressor } from "dyna:compress";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { n++; if (a !== b) { fails++; print("FAIL: " + m + " got " + JSON.stringify(a) + " want " + JSON.stringify(b)); } }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) {
        if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong message: " + e.message); }
    }
}
{
    for (const a of ["gzip","lz4","lz4frame","zstd","brotli","snappy"]) {
        const c = new Compressor({algo:a});
        eq(c.algo, a, "compressor algo getter " + a);
    }
}
if (fails) { print("test_compress_audit_p1: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_compress_audit_p1 failed"); }
print("test_compress_audit_p1: " + n + " assertions, 0 failures");
