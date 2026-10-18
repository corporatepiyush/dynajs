// timeout: 180
// Split from tests/test_data_medium_fixes.js: the encoding sections.
import { StableStringify } from "dyna:encoding";
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
print("== OPT-003: stable stringify sorts many keys without quadratic blowup ==");
{
    const o = {};
    const keys = [];
    for (let i = 4999; i >= 0; i--) { const k = "k" + String(i).padStart(5, "0"); o[k] = i; keys.push(k); }
    keys.sort();
    const t0 = Date.now();
    const s = StableStringify(o);
    const ms = Date.now() - t0;
    const first = s.indexOf('"k00000"');
    const last = s.indexOf('"k04999"');
    ok(first >= 0 && last > first, "sorted output keeps the first key before the last",
        "first=" + first + " last=" + last);
    ok(ms < 2000, "5000 reversed keys stringify well under two seconds", ms + "ms");
}
print("");
print("test_encoding_data_medium_fixes: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_encoding_data_medium_fixes: " + fail + " of " + (pass + fail) + " assertions FAILED");
