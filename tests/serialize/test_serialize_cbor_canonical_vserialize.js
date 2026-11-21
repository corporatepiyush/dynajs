// covers: dyna:serialize.CBORCanonical
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_serialize_cbor_canonical_vserialize");
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

__kitS.section("canonical CBOR sorts short keys first", () => {
    eq(hex(CBORCanonical({ bb: 1, a: 2 })), "a2616102626262" + "01",
       "canonical CBOR sorts short keys first");
    eq(hex(CBORCanonical({ a: 2, bb: 1 })), hex(CBORCanonical({ bb: 1, a: 2 })),
       "whatever order they were written in");
    assert(hex(CBOREncode({ bb: 1, a: 2 })) !== hex(CBORCanonical({ bb: 1, a: 2 })),
       "while the ordinary encoder preserves insertion order");
});

__kitS.section("b < z < ab is BOTH the bytewise and the (len,bytes) order", () => {
    eq(hex(CBORCanonical({ ab: 1, b: 2, z: 3 })), "a3616202617a0362616201",
       "b < z < ab is BOTH the bytewise and the (len,bytes) order");
    eq(hex(CBORCanonical({ "01": 1, "3": 2 })), "a261330262303101",
       "encoded 6133 < 6230.. across length classes");
    eq(j(CBORDecode(bytes("a1810102"))), j({ "1": 2 }),
       "an array key decodes through its string form, never crashes");
});
__kitS.end();

if (fails) {
    print("test_vserialize: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_vserialize failed");
}
print("test_vserialize: " + n + " assertions, 0 failures");
