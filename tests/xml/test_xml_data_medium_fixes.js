// timeout: 180
// Split from tests/test_data_medium_fixes.js: the xml sections.
import { XMLParse } from "dyna:xml";
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
print("== COMPAT-065: XML Char production is enforced ==");
{
    throws(() => XMLParse("<a>&#1;</a>"), "a C0 character reference is refused", /legal XML character/);
    throws(() => XMLParse("<a>&#xB;</a>"), "a vertical-tab reference is refused", /legal XML character/);
    throws(() => XMLParse("<a>\u0001</a>"), "a raw C0 byte in text is refused", /illegal XML character/);
    throws(() => XMLParse("<a b=\"\u0002\"/>"), "a raw C0 byte in an attribute is refused", /illegal XML/);
    ok(XMLParse("<a>\t\r\n</a>") !== null, "tab, CR and LF stay legal");
    ok(XMLParse("<a>&#xD;</a>") !== null, "a CR character reference stays legal");
}
print("");
print("test_xml_data_medium_fixes: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_xml_data_medium_fixes: " + fail + " of " + (pass + fail) + " assertions FAILED");
