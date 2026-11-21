// covers: dyna:encoding.HexEncode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_hex_encode_basex");
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

__kitS.section("base", () => {
    const bases = [
        ["01", 2], ["012", 3], ["0123456789abcdef", 16],
        ["0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ", 36],
        ["123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz", 58],
    ];
    let bad = 0, checked = 0;
    for (const [alpha] of bases) {
        for (let len = 0; len <= 64; len++) {
            const b = new Uint8Array(len);
            for (let i = 0; i < len; i++) b[i] = (i * 37 + len) & 0xff;
            checked++;
            const back = BaseXDecode(BaseXEncode(b, alpha), alpha);
            if (HexEncode(back) !== HexEncode(b)) {
                bad++;
                if (bad < 4) print("  base " + alpha.length + " len " + len + " differs");
            }
        }
        for (const lead of [1, 2, 5]) {
            const b = new Uint8Array(lead + 3);
            b[lead] = 9; b[lead + 1] = 8; b[lead + 2] = 7;
            checked++;
            if (HexEncode(BaseXDecode(BaseXEncode(b, alpha), alpha)) !== HexEncode(b)) bad++;
        }
    }
    assert(bad === 0, "every (base, length) round-trips exactly (" +
                      (checked - bad) + "/" + checked + ")");
    assert(checked === 5 * 68, "the sweep ran every combination");
});
__kitS.end();

eq(HexEncode(BaseXDecode("ff", "0123456789abcdef")), "ff", "base 16 is hex");

if (fails) {
    print("test_basex: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_basex failed");
}
print("test_basex: " + n + " assertions, 0 failures");
