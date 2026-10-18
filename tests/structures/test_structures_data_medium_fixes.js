// timeout: 180
// Split from tests/test_data_medium_fixes.js: the structures sections.
import { Graph } from "dyna:structures";
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
print("== SEC-133: a graph varint delta past UINT32_MAX is refused, not truncated ==");
{
    const payload = [1, 0, ...u32(0xFFFFFFFF), ...u32(2), ...uvar(1), ...uvar(8589934594), ...uvar(0)];
    throws(() => Graph.deserialize(record(14, payload)),
        "an edge delta wrapping past 2^32 is refused", /truncated|malformed|Graph/);
    const sane = [1, 0, ...u32(0xFFFFFFFF), ...u32(2), ...uvar(1), ...uvar(2), ...uvar(0)];
    const g = Graph.deserialize(record(14, sane));
    ok(g && g.nodeCount === 2, "a sane varint graph still decodes", g && String(g.nodeCount));
}
print("");
print("test_structures_data_medium_fixes: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_structures_data_medium_fixes: " + fail + " of " + (pass + fail) + " assertions FAILED");
