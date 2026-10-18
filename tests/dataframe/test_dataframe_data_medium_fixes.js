// timeout: 180
// Split from tests/test_data_medium_fixes.js: the dataframe sections.
import { DataFrame } from "dyna:dataframe";
import { CRC32C } from "dyna:hash";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const throws = (fn, w, re) => {
    try { fn(); ok(false, w, "no throw"); }
    catch (e) { ok(!re || re.test(String(e.message)), w, String(e.message).slice(0, 80)); }
};
function u16(v) { return [v & 0xff, (v >>> 8) & 0xff]; }
function u32(v) { return [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff]; }
function u64(v) { return [...u32(v >>> 0), ...u32(Math.floor(v / 4294967296))]; }
function uvar(v) { const out = []; do { let b = v & 0x7f; v = Math.floor(v / 128); if (v) b |= 0x80; out.push(b); } while (v); return out; }
function record(tid, payload) {
    const head = [0x44, 0x59, 0x4e, 0x53, ...u16(1), ...u16(tid), ...u32(0), ...u64(payload.length)];
    const body = [...head, ...payload];
    const crc = CRC32C(new Uint8Array(body)) >>> 0;
    return new Uint8Array([...body, ...u32(crc)]);
}
print("== SEC-123: MultiMatcher enumeration is budgeted ==");
print("");
print("== COMPAT-070: MELT accepts a repeated value column ==");
{
    const f = new DataFrame({ id: new Int32Array([1, 2]), a: new Float64Array([10, 20]) });
    const m = f.MELT(["id"], ["a", "a"]);
    const c = m.TO_COLUMNS();
    ok(m.ROWS === 4, "a repeated valueVar yields two rows per input row", String(m.ROWS));
    ok(JSON.stringify(Array.from(c.variable)) === JSON.stringify(["a", "a", "a", "a"]),
        "both variable entries name the repeated column", JSON.stringify(Array.from(c.variable)));
    ok(JSON.stringify(Array.from(c.value)) === JSON.stringify([10, 10, 20, 20]),
        "values repeat in valueVars order", JSON.stringify(Array.from(c.value)));
}
print("");
print("test_dataframe_data_medium_fixes: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_dataframe_data_medium_fixes: " + fail + " of " + (pass + fail) + " assertions FAILED");
