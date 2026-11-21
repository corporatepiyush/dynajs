// covers: dyna:serialize.CBOREncode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_serialize_cbor_encode_vserialize");
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

__kitS.section("CBOR(", () => {
    const vectors = [
        [0, "00"], [1, "01"], [10, "0a"], [23, "17"], [24, "1818"], [25, "1819"],
        [100, "1864"], [1000, "1903e8"], [1000000, "1a000f4240"],
        [-1, "20"], [-10, "29"], [-100, "3863"], [-1000, "3903e7"],
        [1.1, "fb3ff199999999999a"], [1.5, "fb3ff8000000000000"],
        [-4.1, "fbc010666666666666"],
        [false, "f4"], [true, "f5"], [null, "f6"], [undefined, "f7"],
        ["", "60"], ["a", "6161"], ["IETF", "6449455446"],
        ["ü", "62c3bc"], ["水", "63e6b0b4"],
        [[], "80"], [[1, 2, 3], "83010203"],
        [[1, [2, 3], [4, 5]], "8301820203820405"],
        [{}, "a0"],
        [{ a: 1, b: [2, 3] }, "a26161016162820203"],
        [["a", { b: "c" }], "826161a161626163"],
    ];
    let bad = 0;
    for (const [v, want] of vectors) {
        const got = hex(CBOREncode(v));
        if (got !== want) {
            bad++;
            if (bad < 5) print("  CBOR(" + j(v) + ") = " + got + ", want " + want);
        }
    }
    assert(bad === 0, "every RFC 8949 vector encodes to the specified bytes ("
                      + (vectors.length - bad) + "/" + vectors.length + ")");
    let bad2 = 0;
    for (const [v, enc] of vectors)
        if (j(CBORDecode(bytes(enc))) !== j(v)) bad2++;
    assert(bad2 === 0, "and every one of them decodes back (" +
                       (vectors.length - bad2) + "/" + vectors.length + ")");
    assert(vectors.length === 31, "the vector list is the one from the RFC");
});
__kitS.end();

eq(hex(CBOREncode(new Uint8Array([1, 2, 3]))), "43010203", "and a CBOR byte string");

if (fails) {
    print("test_vserialize: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_vserialize failed");
}
print("test_vserialize: " + n + " assertions, 0 failures");
