/* test_sha3.js -- SHA-3, Keccak-256 and SHAKE in dyna:hash (design 26).
 *
 * THE ORACLE IS THE PUBLISHED KATs. A hash has no other check worth having:
 * every wrong implementation still returns 32 plausible bytes, and only a
 * known answer distinguishes them. SHA-3 and Keccak differ by ONE padding
 * byte (0x06 against 0x01), so the pair of empty-input vectors below is what
 * proves the two are not silently the same function.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_sha3.js
 */
import { SHA3_224, SHA3_256, SHA3_384, SHA3_512, Keccak256, SHAKE128, SHAKE256,
         SHA3_224Hex, SHA3_256Hex, SHA3_384Hex, SHA3_512Hex, Keccak256Hex,
         SHAKE128Hex, SHAKE256Hex } from "dyna:hash";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + "\n    got  " + a + "\n    want " + b);
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");

/* ------------------------------------------------- the known answers */

{
    const kats = [
        [SHA3_224Hex, "", "6b4e03423667dbb73b6e15454f0eb1abd4597f9a1b078e3f5b5a6bc7"],
        [SHA3_256Hex, "", "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a"],
        [SHA3_256Hex, "abc", "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532"],
        [SHA3_384Hex, "", "0c63a75b845e4f7d01107d852e4c2485c51a50aaaa94fc61995e71bbee983a2a" +
                          "c3713831264adb47fb6bd1e058d5f004"],
        [SHA3_512Hex, "", "a69f73cca23a9ac5c8b567dc185a756e97c982164fe25859e0d1dcc1475c80a6" +
                          "15b2123af1f5f94c11e3e9402c3ac558f500199d95b6d3e301758586281dcd26"],
        /* FIPS 202's other canonical message, alongside SHA3-256("abc") above. */
        [SHA3_512Hex, "abc", "b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e" +
                             "10e116e9192af3c91a7ec57647e3934057340b4cf408d5a56592f8274eec53f0"],
        /* The Ethereum empty hash: Keccak, not SHA-3, and the difference is
         * exactly the padding byte. */
        [Keccak256Hex, "", "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470"],
        [SHAKE128Hex, "", "7f9c2ba4e88f827d616045507605853ed73b8093f6efbc88eb1a6eacfa66ef26"],
    ];
    let bad = 0;
    for (const [fn, input, want] of kats) {
        const got = fn(input);
        if (got !== want) { bad++; print("  " + JSON.stringify(input) + " -> " + got); }
    }
    assert(bad === 0, "every published vector matches (" + (kats.length - bad) +
                      "/" + kats.length + ")");
    assert(kats.length === 8, "all eight vectors ran");
}
/* ONE padding byte apart, and it must actually be apart. */
assert(SHA3_256Hex("") !== Keccak256Hex(""),
       "SHA3-256 and Keccak256 are different functions, not the same one twice");
assert(SHA3_256Hex("abc") !== Keccak256Hex("abc"), "on any input");

/* ---------------------------------------------- shapes and lengths */

eq(SHA3_224("").length, 28, "SHA3-224 is 28 bytes");
eq(SHA3_256("").length, 32, "SHA3-256 is 32");
eq(SHA3_384("").length, 48, "SHA3-384 is 48");
eq(SHA3_512("").length, 64, "SHA3-512 is 64");
eq(Keccak256("").length, 32, "Keccak256 is 32");
assert(SHA3_256("") instanceof Uint8Array, "the byte form is a Uint8Array");
eq(hex(SHA3_256("abc")), SHA3_256Hex("abc"), "and the hex form is its hex");

/* SHAKE squeezes as much as you ask for, and a prefix is stable. */
eq(SHAKE128("", 16).length, 16, "SHAKE128 takes a length");
eq(SHAKE256("", 100).length, 100, "and SHAKE256 does too");
eq(SHAKE128Hex("", 16), SHAKE128Hex("", 32).slice(0, 32),
   "a shorter squeeze is a PREFIX of a longer one -- that is what an XOF means");
eq(SHAKE256Hex("x", 8), SHAKE256Hex("x", 200).slice(0, 16), "for SHAKE256 too");
{
    /* A squeeze longer than the rate crosses a permutation boundary, which is
     * the loop a one-block implementation gets wrong. */
    const long1 = SHAKE128Hex("", 400);
    eq(long1.length, 800, "400 bytes squeezed");
    eq(long1.slice(0, 64), SHAKE128Hex("", 32), "and its head is unchanged");
}

/* -------------------------------------- inputs across the block boundary */

{
    /* The rate is 136 bytes for SHA3-256, so the interesting lengths are the
     * ones either side of it and of the padding's own edge cases. */
    const lens = [0, 1, 55, 63, 64, 71, 72, 135, 136, 137, 200, 271, 272, 1000];
    let bad = 0;
    for (const len of lens) {
        const s = "a".repeat(len);
        const b = new Uint8Array(len);
        for (let i = 0; i < len; i++) b[i] = 97;
        if (SHA3_256Hex(s) !== hex(SHA3_256(b))) bad++;
        if (SHA3_256Hex(s).length !== 64) bad++;
    }
    assert(bad === 0, "a string and the same bytes hash identically at every " +
                      "length around the rate (" + lens.length + " lengths)");
}
eq(SHA3_256Hex(new Uint8Array([97, 98, 99])), SHA3_256Hex("abc"),
   "bytes and their string are the same input");

/* ------------------------------------------------------------ refusals */

throws(() => SHA3_256(), "data is required");
throws(() => SHAKE128("", 0), "a zero-length squeeze");
throws(() => SHAKE128("", -1), "a negative one");
throws(() => SHAKE128("", 1 << 21), "and an unreasonable one");

if (fails) {
    print("test_sha3: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_sha3 failed");
}
print("test_sha3: " + n + " assertions, 0 failures");
