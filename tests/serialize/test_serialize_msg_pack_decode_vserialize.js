// covers: dyna:serialize.MsgPackDecode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_serialize_msg_pack_decode_vserialize");
import { MsgPackEncode, MsgPackDecode, CBOREncode, CBORDecode,
         CBORCanonical, ValueHash, structuredClone } from "dyna:serialize";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
const bytes = (h) => new Uint8Array(h.match(/../g) ? h.match(/../g).map((x) => parseInt(x, 16)) : []);
const j = (v) => JSON.stringify(v);

__kitS.section("a 70k array (32-bit prefix)", () => {
    const arr = [], obj = {};
    for (let i = 0; i < 70000; i++) arr.push(i % 250);
    for (let i = 0; i < 100; i++) obj["k" + i] = i;
    eq(MsgPackDecode(MsgPackEncode(arr)).length, 70000, "a 70k array (32-bit prefix)");
    eq(CBORDecode(CBOREncode(arr)).length, 70000, "in CBOR too");
    eq(Object.keys(MsgPackDecode(MsgPackEncode(obj))).length, 100, "a 100-key map");
    eq(MsgPackDecode(MsgPackEncode("y".repeat(70000))).length, 70000, "a 70k string");
});

throws(() => MsgPackDecode(), "and bytes to decode");

throws(() => MsgPackDecode("not bytes"), "which must be bytes");

throws(() => MsgPackDecode(bytes("9301")), "a truncated array is refused");

throws(() => MsgPackDecode(bytes("a5ab")), "and a truncated string");

throws(() => MsgPackDecode(bytes("0101")), "trailing bytes after the value");

throws(() => MsgPackDecode(bytes("c1")), "an unassigned type byte");

__kitS.section("400-deep nesting is refused", () => {
    let deep = "";
    for (let i = 0; i < 400; i++) deep += "91";
    deep += "01";
    throws(() => MsgPackDecode(bytes(deep)), "400-deep nesting is refused");
    let ok = "";
    for (let i = 0; i < 100; i++) ok += "91";
    ok += "01";
    eq(MsgPackDecode(bytes(ok))[0][0].length, 1, "and 100 deep still decodes");
});

__kitS.section("__proto__ is an own key in the result", () => {
    const o = MsgPackDecode(MsgPackEncode({ ["__proto__"]: "x", b: 1 }));
    eq(Object.keys(o).length, 2, "__proto__ is an own key in the result");
    eq(o.__proto__, "x", "with its value");
});
__kitS.end();

if (fails) {
    print("test_vserialize: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_vserialize failed");
}
print("test_vserialize: " + n + " assertions, 0 failures");
