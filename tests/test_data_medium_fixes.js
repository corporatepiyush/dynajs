// timeout: 180
import { MultiMatcher } from "dyna:matcher";
import { Graph } from "dyna:structures";
import { Parse as YAMLParse } from "dyna:yaml";
import { XMLParse } from "dyna:xml";
import { Proto } from "dyna:serialize";
import { StableStringify } from "dyna:encoding";
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
{
    const mm = new MultiMatcher(["a", "aa"]);
    ok(mm.countIn("aa") === 3, "countIn still counts every hit on short text", String(mm.countIn("aa")));
    ok(mm.firstIn("baa") !== null && mm.firstIn("baa") !== undefined, "firstIn still finds a hit");
    ok(mm.countIn("a".repeat(1200000)) === 2399999, "countIn still sums dense hits below the step budget",
        String(mm.countIn("a".repeat(1200000))));
    throws(() => mm.allIn("a".repeat(1200000)), "a dense 1.2M-char allIn stops at the hit budget",
        /budget|dense/);
}

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
print("== COMPAT-064: YAML flow scalars stop at comments ==");
{
    throws(() => YAMLParse("[1, # c]"), "a comment inside an unclosed flow sequence is refused", /comment/);
    throws(() => YAMLParse("{a: 1 # c}"), "a comment inside an unclosed flow mapping is refused", /comment/);
    const doc = YAMLParse("a: [1, 2] # trailing\nb: {x: y}\n");
    ok(doc && doc.a && doc.a.length === 2 && doc.a[0] === 1, "a comment after a closed collection still parses",
        JSON.stringify(doc));
    ok(doc && doc.b && doc.b.x === "y", "block mapping after a flow value still parses");
}

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
print("test_data_medium_fixes: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_data_medium_fixes: " + fail + " of " + (pass + fail) + " assertions FAILED");
