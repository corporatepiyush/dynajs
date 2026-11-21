// covers: dyna:encoding.Base58CheckDecode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_base58_check_decode_basex");
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

__kitS.section("the canonical address", () => {
    const payload = "00010966776006953D5567439E5E39F86A0D273BEE";
    const addr = "16UwLL9Risc3QfPqBUvKofHmBQ7wMtjvM";
    eq(Base58CheckEncode(HexDecode(payload)), addr, "the canonical address");
    eq(HexEncode(Base58CheckDecode(addr)), payload.toLowerCase(),
       "and it decodes back to the payload without the checksum");
    let caught = 0, tried = 0;
    for (let i = 1; i < addr.length; i++) {
        const c = addr[i];
        const alt = c === "1" ? "2" : "1";
        tried++;
        try { Base58CheckDecode(addr.slice(0, i) + alt + addr.slice(i + 1)); }
        catch (e) { caught++; }
    }
    assert(caught === tried, "every one-character corruption is rejected ("
                             + caught + "/" + tried + ")");
});
__kitS.end();

throws(() => Base58CheckDecode("111"), "too short to carry a checksum");

if (fails) {
    print("test_basex: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_basex failed");
}
print("test_basex: " + n + " assertions, 0 failures");
