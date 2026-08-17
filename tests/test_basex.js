/* test_basex.js -- Base58, Base58Check and BaseX in dyna:encoding (design 26).
 *
 * THE ORACLE IS THE BITCOIN TEST VECTORS, which every base58 implementation is
 * checked against, plus a round trip over every byte length from 0 to 64 in
 * several bases. A division codec's bugs live at the boundaries -- leading
 * zeros, the buffer bound, and a base that is not 58.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_basex.js
 */
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

/* --------------------------- the published base58 vectors, both ways */

{
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
}
eq(Base58Encode("hello world"), "StV1DL6CwTryKyV", "a string input is its UTF-8");
eq(Base58Encode(new Uint8Array([0, 0, 1])), "112",
   "each LEADING ZERO byte is one leading '1', which is the whole point of the format");

/* ------------------------------------------------- Base58Check */

{
    /* The canonical worked example: version byte + hash160 of an address. */
    const payload = "00010966776006953D5567439E5E39F86A0D273BEE";
    const addr = "16UwLL9Risc3QfPqBUvKofHmBQ7wMtjvM";
    eq(Base58CheckEncode(HexDecode(payload)), addr, "the canonical address");
    eq(HexEncode(Base58CheckDecode(addr)), payload.toLowerCase(),
       "and it decodes back to the payload without the checksum");
    /* A SINGLE mistyped character is what the checksum exists to catch. */
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
}
throws(() => Base58CheckDecode("111"), "too short to carry a checksum");
throws(() => Base58Decode("0OIl"), "characters base58 deliberately omits");
throws(() => Base58Decode(new Uint8Array([1])), "decode takes a string");

/* ------------------------------------ round trips across bases and lengths */

{
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
        /* leading zeros are the case a division codec loses */
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
}
eq(BaseXEncode(HexDecode("ff00"), "01"), "1111111100000000",
   "base 2 is eight characters per byte, which the size bound must allow for");
eq(HexEncode(BaseXDecode("ff", "0123456789abcdef")), "ff", "base 16 is hex");

/* --------------------------------------------------------------- refusals */

throws(() => BaseXEncode("x", "aab"), "a repeated character corrupts decoding");
throws(() => BaseXEncode("x", "a"), "a one-character alphabet");
throws(() => BaseXEncode("x", ""), "an empty one");
throws(() => BaseXDecode("zzz", "01"), "a character outside the alphabet");
throws(() => BaseXEncode("x"), "an alphabet is required");
throws(() => BaseXDecode(new Uint8Array([1]), "01"), "decode takes a string");
{
    /* The cap is the defence: this is a quadratic codec. */
    const big = new Uint8Array(5000);
    throws(() => Base58Encode(big), "past the input cap it refuses rather than churns");
    throws(() => Base58Decode("1".repeat(5000)), "in both directions");
}

if (fails) {
    print("test_basex: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_basex failed");
}
print("test_basex: " + n + " assertions, 0 failures");
