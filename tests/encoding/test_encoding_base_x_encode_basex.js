// covers: dyna:encoding.BaseXEncode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_base_x_encode_basex");
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

__kitS.section("base 2 is eight characters per byte, which the size bound must allow for", () => {
eq(BaseXEncode(HexDecode("ff00"), "01"), "1111111100000000",
   "base 2 is eight characters per byte, which the size bound must allow for");
});

__kitS.section("a repeated character corrupts decoding", () => {
throws(() => BaseXEncode("x", "aab"), "a repeated character corrupts decoding");
});

__kitS.section("a one-character alphabet", () => {
throws(() => BaseXEncode("x", "a"), "a one-character alphabet");
});

__kitS.section("an empty one", () => {
throws(() => BaseXEncode("x", ""), "an empty one");
});

__kitS.section("an alphabet is required", () => {
throws(() => BaseXEncode("x"), "an alphabet is required");
});
__kitS.end();

if (fails) {
    print("test_basex: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_basex failed");
}
print("test_basex: " + n + " assertions, 0 failures");
