// covers: dyna:encoding.Base58Encode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_base58_encode_basex");
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

__kitS.section(", want", () => {
    const vectors = [
        ["", ""], ["61", "2g"], ["626262", "a3gV"], ["636363", "aPEr"],
        ["73696d706c792061206c6f6e6720737472696e67",
         "2cFupjhnEsSn59qHXstmK2ffpLv2"],
        ["00eb15231dfceb60925886b67d065299925915aeb172c06647",
         "1NS17iag9jJgTHD1VXjvLCEnZuQ3rJDE9L"],
        ["516b6fcd0f", "ABnLTmg"], ["bf4f89001e670274dd", "3SEo3LWLoPntC"],
        ["572e4794", "3EFU7m"], ["ecac89cad93923c02321", "EJDM8drfXA6uyA"],
        ["10c8511e", "Rt5zm"],
        ["00000000000000000000", "1111111111"],
        ["00", "1"], ["0000", "11"], ["000001", "112"],
    ];
    let bad = 0;
    for (const [hex, want] of vectors) {
        const got = Base58Encode(HexDecode(hex));
        if (got !== want) { bad++; print("  " + hex + " -> " + got + ", want " + want); }
        const back = HexEncode(Base58Decode(want));
        if (back !== hex) { bad++; print("  " + want + " -> " + back + ", want " + hex); }
    }
    assert(bad === 0, "every published base58 vector matches, both directions ("
                      + (vectors.length * 2 - bad) + "/" + (vectors.length * 2) + ")");
    assert(vectors.length === 15, "the vector list is the published one");
});

eq(Base58Encode("hello world"), "StV1DL6CwTryKyV", "a string input is its UTF-8");

eq(Base58Encode(new Uint8Array([0, 0, 1])), "112",
   "each LEADING ZERO byte is one leading '1', which is the whole point of the format");

__kitS.section("past the input cap it refuses rather than churns", () => {
    const big = new Uint8Array(5000);
    throws(() => Base58Encode(big), "past the input cap it refuses rather than churns");
    throws(() => Base58Decode("1".repeat(5000)), "in both directions");
});
__kitS.end();

if (fails) {
    print("test_basex: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_basex failed");
}
print("test_basex: " + n + " assertions, 0 failures");
