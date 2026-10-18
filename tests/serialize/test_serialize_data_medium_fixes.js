// timeout: 180
// Split from tests/test_data_medium_fixes.js: the serialize sections.
import { Proto } from "dyna:serialize";
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
print("== COMPAT-066: protobuf map keys do not saturate through strtoll ==");
{
    const U64 = { fields: [{ name: "m", number: 1, type: "message", map: true, keyType: "uint64", valueType: "int32" }] };
    throws(() => Proto.encode({ m: { "18446744073709551615": 1 } }, U64),
        "a uint64 key above INT64_MAX is refused", /not an integer|out of range/);
    throws(() => Proto.encode({ m: { "-9223372036854775809": 1 } }, U64),
        "a negative int64 key below INT64_MIN is refused", /not an integer|out of range/);
    const ok64 = Proto.encode({ m: { "9223372036854775807": 1 } }, U64);
    ok(ok64 && ok64.length > 0, "INT64_MAX itself still encodes");
}
print("");
print("test_serialize_data_medium_fixes: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_serialize_data_medium_fixes: " + fail + " of " + (pass + fail) + " assertions FAILED");
