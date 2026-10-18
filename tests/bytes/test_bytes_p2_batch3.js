// Split from tests/test_p2_batch3.js: the bytes sections.
import { Bytes } from "dyna:bytes";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) { if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); } }
}
{
    const b = new Bytes(new Uint8Array([1, 2, 3, 4]));
    const r = b.fill(9, 1, 3);
    assert(r === b, "Bytes.fill returns the handle");
    assert(r instanceof Bytes, "Bytes.fill result is a Bytes");
    assert(b.readUint8(0) === 1 && b.readUint8(1) === 9 && b.readUint8(2) === 9 && b.readUint8(3) === 4,
           "Bytes.fill mutated correctly");
}
if (fails) { print("test_bytes_p2_batch3: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_bytes_p2_batch3 failed"); }
print("test_bytes_p2_batch3: " + n + " assertions, 0 failures");
