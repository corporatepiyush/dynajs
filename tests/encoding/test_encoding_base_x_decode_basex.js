// covers: dyna:encoding.BaseXDecode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_base_x_decode_basex");
import { Base58Encode, Base58Decode, Base58CheckEncode, Base58CheckDecode,
         BaseXEncode, BaseXDecode, HexEncode, HexDecode } from "dyna:encoding";

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

__kitS.section("a character outside the alphabet", () => {
throws(() => BaseXDecode("zzz", "01"), "a character outside the alphabet");
});

__kitS.section("decode takes a string", () => {
throws(() => BaseXDecode(new Uint8Array([1]), "01"), "decode takes a string");
});
__kitS.end();

if (fails) {
    print("test_basex: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_basex failed");
}
print("test_basex: " + n + " assertions, 0 failures");
