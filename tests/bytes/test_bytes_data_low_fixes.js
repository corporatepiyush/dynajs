// timeout: 180
// Split from tests/test_data_low_fixes.js: the bytes sections.
import { Bytes } from "dyna:bytes";
import { sniffType } from "dyna:file";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const throws = (fn, w, re) => {
    try { fn(); ok(false, w, "no throw"); }
    catch (e) { ok(!re || re.test(String(e.message)), w, String(e.message).slice(0, 80)); }
};
const E = (y, m, d) => Date.UTC(y, m - 1, d) / 1000;
print("== COMPAT-225/226: JSON5 number grammar ==");
print("");
print("== SEC-175 / OPT-036: byte helpers ==");
{
    const p = new Proxy([], { get(t, k) { if (k === "length") return 9007199254740992; return new Uint8Array(1); } });
    throws(() => Bytes.concat(p), "Bytes.concat with a huge proxy length is refused", /list length/);
    ok(Bytes.concat([new Uint8Array([1, 2]), new Uint8Array([3])]).length === 3,
        "a normal concat still works");
    const big = new Uint8Array(1 << 20).fill(65);
    ok(sniffType(big) === "text/plain", "sniffType caps its NUL scan at the head window", sniffType(big));
}
print("");
print("test_bytes_data_low_fixes: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_bytes_data_low_fixes: " + fail + " of " + (pass + fail) + " assertions FAILED");
