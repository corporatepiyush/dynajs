// Black-box contract test for dyna:hash, generated from dynajs.d.ts lines 1476-1592. Engine sources not consulted; every expectation cites the contract.
import {
    MD5, MD5Hex, SHA1, SHA1Hex, SHA224, SHA224Hex, SHA256, SHA256Hex, SHA384, SHA384Hex,
    SHA512, SHA512Hex, CRC32, CRC32C,
    SHA3_224, SHA3_224Hex, SHA3_256, SHA3_256Hex, SHA3_384, SHA3_384Hex, SHA3_512, SHA3_512Hex,
    Keccak256, Keccak256Hex, SHAKE128, SHAKE128Hex, SHAKE256, SHAKE256Hex,
    BLAKE3, BLAKE3Hex, BLAKE2b, BLAKE2bHex, BLAKE2s, BLAKE2sHex,
    Murmur3_128, Murmur3_128Hex, XXHash32, XXHash64, XXH3_64, Hasher,
} from "dyna:hash";
import { HexEncode } from "dyna:encoding";
import { fromUtf8 } from "dyna:bytes";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg + " — got |" + JSON.stringify(a) + "| expected |" + JSON.stringify(b) + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
function eqArr(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
function u8(...bytes) { return new Uint8Array(bytes); }
function zeros(k) { return new Uint8Array(k); }
function J(v) { return typeof v === "bigint" ? v + "n" : JSON.stringify(v); }

/* Table runners: rows are [args..., expected] (expected may be a predicate) or [args..., ErrorClass]. */
function labelOf(name, args) {
    return name + "(" + args.map(a => typeof a === "string" ? JSON.stringify(a.length > 16 ? a.slice(0, 16) + "…" : a) : (a instanceof Uint8Array ? "u8[" + a.length + "]" : String(a))).join(", ") + ")";
}
function assertCases(fn, cases, name) {
    for (const row of cases) {
        const args = row.slice(0, row.length - 1);
        const want = row[row.length - 1];
        n++;
        let got, threw = null;
        try { got = fn(...args); } catch (e) { threw = e; }
        const label = labelOf(name, args);
        if (threw) throw new Error("case threw " + threw + ": " + label);
        if (typeof want === "function") { if (!want(got)) throw new Error("case predicate failed: " + label + " — got |" + J(got) + "|"); }
        else if (J(got) !== J(want)) throw new Error("case failed: " + label + " — got |" + J(got) + "| expected |" + J(want) + "|");
    }
}
function assertCasesThrow(fn, cases, name) {
    for (const row of cases) {
        const args = row.slice(0, row.length - 1);
        const Err = row[row.length - 1];
        n++;
        let threw = false, err = null;
        try { fn(...args); } catch (e) { threw = true; err = e; }
        const label = labelOf(name, args);
        if (!threw) throw new Error("expected throw: " + label);
        if (Err && !(err instanceof Err)) throw new Error("wrong error class " + (err && err.constructor ? err.constructor.name : String(err)) + ": " + label);
    }
}

/* ---------------- one-shot digest vectors ---------------- */
/* One table, one loop: [HexFn, input, expected hex, vector source]. */
{
    const VECTORS = [
        // RFC 1321 test suite
        [MD5Hex, "", "d41d8cd98f00b204e9800998ecf8427e", "RFC 1321"],
        [MD5Hex, "abc", "900150983cd24fb0d6963f7d28e17f72", "RFC 1321"],
        [MD5Hex, "message digest", "f96b697d7cb7938d525a2f31aaf161d0", "RFC 1321"],
        [MD5Hex, "abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b", "RFC 1321"],
        // FIPS 180-4
        [SHA1Hex, "", "da39a3ee5e6b4b0d3255bfef95601890afd80709", "FIPS 180-4"],
        [SHA1Hex, "abc", "a9993e364706816aba3e25717850c26c9cd0d89d", "FIPS 180-4"],
        [SHA224Hex, "", "d14a028c2a3a2bc9476102bb288234c415a2b01f828ea62ac5b3e42f", "FIPS 180-4"],
        [SHA224Hex, "abc", "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7", "FIPS 180-4"],
        [SHA256Hex, "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "FIPS 180-4"],
        [SHA256Hex, "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "FIPS 180-4 / API-pinned"],
        [SHA384Hex, "", "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b", "FIPS 180-4"],
        [SHA384Hex, "abc", "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7", "FIPS 180-4"],
        [SHA512Hex, "", "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e", "FIPS 180-4"],
        [SHA512Hex, "abc", "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", "FIPS 180-4"],
        // FIPS 202 SHA-3
        [SHA3_224Hex, "", "6b4e03423667dbb73b6e15454f0eb1abd4597f9a1b078e3f5b5a6bc7", "FIPS 202"],
        [SHA3_224Hex, "abc", "e642824c3f8cf24ad09234ee7d3c766fc9a3a5168d0c94ad73b46fdf", "FIPS 202"],
        [SHA3_256Hex, "", "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a", "FIPS 202"],
        [SHA3_256Hex, "abc", "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532", "FIPS 202 / API-pinned prefix"],
        [SHA3_384Hex, "", "0c63a75b845e4f7d01107d852e4c2485c51a50aaaa94fc61995e71bbee983a2ac3713831264adb47fb6bd1e058d5f004", "FIPS 202"],
        [SHA3_384Hex, "abc", "ec01498288516fc926459f58e2c6ad8df9b473cb0fc08c2596da7cf0e49be4b298d88cea927ac7f539f1edf228376d25", "FIPS 202"],
        [SHA3_512Hex, "", "a69f73cca23a9ac5c8b567dc185a756e97c982164fe25859e0d1dcc1475c80a615b2123af1f5f94c11e3e9402c3ac558f500199d95b6d3e301758586281dcd26", "FIPS 202"],
        // Keccak (original padding, Ethereum's form); API.md pins the empty prefix c5d2460186f7233c
        [Keccak256Hex, "", "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470", "Keccak-256 team vector"],
        [Keccak256Hex, "abc", "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45", "Keccak-256 team vector"],
        // BLAKE2 official test vectors
        [BLAKE2bHex, "", "786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419d25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce", "BLAKE2 official"],
        [BLAKE2bHex, "abc", "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923", "RFC 7693 App. B"],
        [BLAKE2sHex, "", "69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9", "BLAKE2 official"],
        [BLAKE2sHex, "abc", "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982", "RFC 7693 App. B"],
        // BLAKE3 official test vectors
        [BLAKE3Hex, "", "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262", "BLAKE3 official"],
        [BLAKE3Hex, "abc", "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85", "BLAKE3 official"],
    ];
    for (const [fn, input, expected, source] of VECTORS) {
        const got = fn(input);
        if (got !== expected) throw new Error("case failed: vector " + (fn.name || "hex") + "(" + JSON.stringify(input) + ") [" + source + "] — got |" + got + "| expected |" + expected + "|");
        n++;
    }
    // d.ts: "Keccak256 ... Original Keccak padding, the form Ethereum uses" — a different padding
    // than SHA3-256, so the same input must NOT digest the same.
    assertCases((s) => eqArr(Array.from(Keccak256(s)), Array.from(SHA3_256(s))), [
        ["abc", false],
        ["", false],
    ], "Keccak256 vs SHA3_256 padding");
}

/* ---------------- raw-digest forms, output lengths, input forms ---------------- */
{
    // d.ts: the * functions return Uint8Array; *Hex returns the hex of the same bytes.
    assertCases((input) => [MD5(input) instanceof Uint8Array, MD5(input).length, MD5Hex(input), HexEncode(MD5(input))], [
        // cross-checked against dyna:encoding HexEncode — *Hex is the hex of the * bytes
        // (is-a check is Uint8Array, not Array.isArray: d.ts pins the return type as Uint8Array)
        ["", [true, 16, "d41d8cd98f00b204e9800998ecf8427e", "d41d8cd98f00b204e9800998ecf8427e"]],
    ], "MD5/MD5Hex");
    assertCases((input) => SHA256(input).length, [
        ["abc", 32],
        [u8(0x61, 0x62, 0x63), 32],
    ], "SHA256 length");
    // d.ts: data is BytesInput — a string is its UTF-8 bytes; a byte view / ArrayBuffer identical
    assertCases((make) => make(), [
        [() => eqArr(Array.from(SHA256("abc")), Array.from(SHA256(fromUtf8("abc")))), true],
        [() => eqArr(Array.from(SHA256(fromUtf8("abc"))), Array.from(SHA256(fromUtf8("abc").buffer))), true],
        [() => eqArr(Array.from(MD5(u8())), Array.from(MD5(""))), true],
    ], "BytesInput forms");
    // parametric digest-size table (d.ts: MD5..SHA3_512..BLAKE2 fixed widths)
    const SIZES = [
        [MD5, 16], [SHA1, 20], [SHA224, 28], [SHA256, 32], [SHA384, 48], [SHA512, 64],
        [SHA3_224, 28], [SHA3_256, 32], [SHA3_384, 48], [SHA3_512, 64],
        [Keccak256, 32], [BLAKE2s, 32], [BLAKE2b, 64], [BLAKE3, 32],
    ];
    for (const [fn, size] of SIZES) {
        assertEq(fn("abc").length, size, (fn.name || "digest") + ": output length " + size + " (d.ts)");
    }
}

/* ---------------- CRC ---------------- */
{
    // d.ts: "IEEE 802.3 CRC-32 as a non-negative number"; CRC-32C Castagnoli.
    assertCases((...a) => CRC32(...a), [
        ["123456789", 3421780262],           // 0xCBF43926, the IEEE check value
        ["abc", 891568578],                  // API-pinned
    ], "CRC32");
    assertCases((...a) => CRC32C(...a), [
        ["123456789", 3808858755],           // 0xE3069283, the Castagnoli check value
        ["abc", 910901175],                  // API-pinned
    ], "CRC32C");
    assertCases((input) => [typeof CRC32(input), CRC32(input) > 0], [
        // a result above 2^31 stays a positive number (d.ts: "as a non-negative number")
        ["123456789", ["number", true]],
    ], "CRC32 shape");
    assertCases((input) => CRC32(input) === CRC32(fromUtf8(input)), [
        ["123456789", true],                 // string input is its UTF-8 bytes
    ], "CRC32 input form");
}

/* ---------------- SHAKE extensible output ---------------- */
{
    // d.ts: "SHAKE128 extensible output; length 1..2^20 bytes" (default 32 — API.md)
    assertCases((...a) => SHAKE128Hex(...a), [
        ["", "7f9c2ba4e88f827d616045507605853ed73b8093f6efbc88eb1a6eacfa66ef26"],   // SHAKE128("") 32-byte official value
        ["", 16, "7f9c2ba4e88f827d616045507605853e"],    // XOF prefix property
    ], "SHAKE128Hex");
    assertCases((...a) => SHAKE256Hex(...a), [
        ["", "46b9dd2b0ba88d13233b3feb743eeb243fcd52ea62b81b82b50c27646ed5762f"],   // SHAKE256("") 32-byte official value
    ], "SHAKE256Hex");
    assertCases((len) => SHAKE128("abc", len).length, [
        [1, 1],
        [32, 32],
        [1000, 1000],
    ], "SHAKE128 length");
    // XOF property: a shorter output is a prefix of a longer one (Keccak XOF definition)
    assertCases((data) => eqArr(Array.from(SHAKE256(data, 8)), Array.from(SHAKE256(data, 64).slice(0, 8))), [
        ["abc", true],
    ], "SHAKE256 XOF prefix");
    assertCasesThrow((len) => SHAKE128("abc", len), [
        [0, null],               // length 1..2^20 (d.ts)
        [2 ** 20 + 1, null],
    ], "SHAKE128 length bound");
}

/* ---------------- BLAKE2 keyed / length forms ---------------- */
{
    // RFC 7693 / official BLAKE2 keyed KAT vectors: key is the INCREASING byte
    // sequence 0x00..0x3f (b) / 0x00..0x1f (s), empty message. (The original rows
    // used all-zero keys — those are not the KAT keys.)
    const seq = (k) => { const a = new Uint8Array(k); for (let i = 0; i < k; i++) a[i] = i; return a; };
    assertCases((key) => BLAKE2bHex(zeros(0), { key }), [
        [seq(64), "10ebb67700b1868efb4417987acf4690ae9d972fb7a590c2f02871799aaa4786b5e996e8f0f4eb981fc214b005f42d2ff4233499391653df7aefcbc13fc51568"],
    ], "BLAKE2b keyed (RFC 7693)");
    assertCases((key) => BLAKE2sHex(zeros(0), { key }), [
        [seq(32), "48a8997da407876b3d79c0d92325ad3b89cbb754d86ab71aee047ad345fd2c49"],
    ], "BLAKE2s keyed (RFC 7693)");
    // d.ts: "an EMPTY key is legal and means unkeyed (RFC 7693 -- it is the parameter block's
    // key_length field, not a flag)"
    assertCases((input) => BLAKE2bHex(input, { key: zeros(0) }) === BLAKE2bHex(input), [
        ["abc", true],
        ["", true],
    ], "BLAKE2b empty key = unkeyed");
    assertCases((input) => BLAKE2sHex(input, { key: zeros(0) }) === BLAKE2sHex(input), [
        ["abc", true],
    ], "BLAKE2s empty key = unkeyed");
    // d.ts: "BLAKE2b; 1..64 bytes" / "BLAKE2s; 1..32 bytes"; key lengths 0..64 / 0..32
    assertCases((len) => BLAKE2b("abc", len).length, [
        [1, 1], [16, 16], [64, 64], [undefined, 64],
    ], "BLAKE2b length form");
    assertCases((len) => BLAKE2s("abc", len).length, [
        [32, 32], [undefined, 32],
    ], "BLAKE2s length form");
    assertCasesThrow((...a) => BLAKE2b(...a), [
        ["abc", 0, null],
        ["abc", 65, null],
    ], "BLAKE2b length bound");
    assertCasesThrow((...a) => BLAKE2s(...a), [
        ["abc", 33, null],
    ], "BLAKE2s length bound");
    assertCasesThrow((input, opts) => BLAKE2b(input, opts), [
        ["abc", { key: zeros(65) }, null],   // a key over 64 bytes refuses (API.md)
    ], "BLAKE2b key bound");
    assertCasesThrow((input, opts) => BLAKE2s(input, opts), [
        ["abc", { key: zeros(33) }, null],
    ], "BLAKE2s key bound");
    // keyed output differs from unkeyed
    assertCases((input) => BLAKE2bHex(input, { key: zeros(64) }) !== BLAKE2bHex(input), [
        ["abc", true],
    ], "BLAKE2b keyed differs");
}

/* ---------------- BLAKE3 keyed / derive-key ---------------- */
{
    // d.ts: BLAKE3 keys are EXACTLY 32 bytes; {context}/{deriveKey} is the same option, two
    // spellings; derive output is exactly 32 bytes and `length` is refused there.
    const key = u8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31);
    assertCases((input, opts) => BLAKE3Hex(input, opts), [
        ["abc", { key }, (hex) => typeof hex === "string" && hex.length === 64],
    ], "BLAKE3 keyed");
    assertCasesThrow((input, opts) => BLAKE3(input, opts), [
        ["abc", { key: zeros(0) }, null],    // an empty key refuses for BLAKE3 (API.md)
        ["abc", { key: zeros(31) }, null],
        ["abc", { key: zeros(33) }, null],
    ], "BLAKE3 key bound");
    assertCases((input, opts) => eqArr(Array.from(BLAKE3(input, opts)), Array.from(BLAKE3(input, { deriveKey: "ctx" }))), [
        // {context} and {deriveKey} are the same option (d.ts)
        ["abc", { context: "ctx" }, true],
    ], "BLAKE3 derive-key alias");
    assertCases((input, opts) => BLAKE3(input, opts).length, [
        ["abc", { context: "ctx" }, 32],     // the derive output is exactly 32 bytes (d.ts)
    ], "BLAKE3 derive length");
    assertCasesThrow((input, opts) => BLAKE3(input, opts), [
        ["abc", { context: "ctx", length: 64 }, null],   // `length` is refused in derive mode (d.ts)
    ], "BLAKE3 derive length refusal");
    assertCases((input) => eqArr(Array.from(BLAKE3(input, { context: "ctx" })), Array.from(BLAKE3(input))), [
        ["abc", false],   // derive-key output differs from the plain hash (d.ts: DERIVE_KEY_MATERIAL pass)
    ], "BLAKE3 derive vs plain");
    // d.ts: "length 1..2^20 bytes" extendable output with the XOF prefix property
    assertCases((len) => BLAKE3("abc", len).length, [
        [1, 1], [32, 32], [64, 64], [undefined, 32],
    ], "BLAKE3 length form");
    assertCases(() => eqArr(Array.from(BLAKE3("abc", 64).slice(0, 32)), Array.from(BLAKE3("abc"))), [
        [undefined, true],
    ], "BLAKE3 XOF prefix");
    assertCasesThrow((...a) => BLAKE3(...a), [
        ["abc", 0, null],
        ["abc", 2 ** 20 + 1, null],
    ], "BLAKE3 length bound");
}

/* ---------------- xxHash family ---------------- */
{
    // d.ts: XXHash32 "32-bit xxHash as a number"
    assertCases((...a) => XXHash32(...a), [
        ["abc", 852579327],          // API-pinned
        ["", 46947589],              // 0x02CC5D05, the XXH32 spec check value (seed 0, empty input)
    ], "XXHash32");
    assertCases((seed) => typeof XXHash32("abc", seed), [
        [0, "number"],
        [1, "number"],
    ], "XXHash32 shape");
    assertCases((seed) => XXHash32("abc", seed) !== XXHash32("abc", 0), [
        [1, true],                   // the seed participates
    ], "XXHash32 seed");
    // d.ts: XXHash64 "a 16-character hex string by default"; {as:"bigint"|"bytes"} for the exact
    // value or its little-endian bytes.
    assertCases((...a) => XXHash64(...a), [
        ["", "ef46db3751d8e999"],            // XXH64 spec check value (empty, seed 0)
        ["", 0, { as: "bigint" }, 0xef46db3751d8e999n],
        // expected as predicate: a Uint8Array JSON-stringifies as {"0":..}, so compare element-wise
        ["", 0, { as: "bytes" }, (v) => v instanceof Uint8Array && eqArr(Array.from(v), [0x99, 0xE9, 0xD8, 0x51, 0x37, 0xDB, 0x46, 0xEF])],   // LE, the format's own byte order
    ], "XXHash64");
    assertCases((input) => XXHash64(input).length, [
        ["abc", 16],             // API-pinned
        ["hello world", 16],
    ], "XXHash64 default hex length");
    assertCases((input, seed) => typeof XXHash64(input, seed, { as: "bigint" }), [
        ["abc", 0, "bigint"],
    ], "XXHash64 as bigint");
    // hex form, bigint form and LE bytes form all describe the same 64 bits
    assertCases((input) => BigInt("0x" + XXHash64(input).toLowerCase()) === XXHash64(input, 0, { as: "bigint" }), [
        ["abc", true],
        ["", true],
    ], "XXHash64 hex/bigint parity");
    assertCases((input) => eqArr(Array.from(XXHash64(input, 0, { as: "bytes" })), [
        Number(XXHash64(input, 0, { as: "bigint" }) & 0xFFn),
        Number((XXHash64(input, 0, { as: "bigint" }) >> 8n) & 0xFFn),
        Number((XXHash64(input, 0, { as: "bigint" }) >> 16n) & 0xFFn),
        Number((XXHash64(input, 0, { as: "bigint" }) >> 24n) & 0xFFn),
        Number((XXHash64(input, 0, { as: "bigint" }) >> 32n) & 0xFFn),
        Number((XXHash64(input, 0, { as: "bigint" }) >> 40n) & 0xFFn),
        Number((XXHash64(input, 0, { as: "bigint" }) >> 48n) & 0xFFn),
        Number((XXHash64(input, 0, { as: "bigint" }) >> 56n) & 0xFFn),
    ]), [
        ["abc", true],
    ], "XXHash64 bytes = LE of bigint");
    // d.ts: XXH3_64 "the modern one-shot cousin of XXHash64", same opts
    assertCases((...a) => XXH3_64(...a), [
        ["", 0, { as: "bigint" }, 0x2d06800538d394c2n],   // XXH3-64 spec check value (empty, seed 0)
        ["hello world", (v) => typeof v === "string" && v.length === 16],   // API-pinned default shape
    ], "XXH3_64");
    assertCases((input, seed) => XXH3_64(input, seed, { as: "bytes" }).length, [
        ["hello world", 7, 8],       // API-pinned
    ], "XXH3_64 as bytes");
}

/* ---------------- Murmur3_128 ---------------- */
{
    // d.ts: "Murmur3_128; the second argument is a seed, not a length."
    assertCases((input) => Murmur3_128Hex(input), [
        ["", "00000000000000000000000000000000"],   // murmur3 of the empty input is all zeros
    ], "Murmur3_128Hex empty");
    assertCases((input, seed) => [Murmur3_128(input).length, Murmur3_128Hex(input, seed).length], [
        // 128-bit = 16 bytes = 32 hex chars — proof the second argument is not a length
        ["abc", undefined, [16, 32]],
        ["abc", 7, [16, 32]],
    ], "Murmur3_128 shape");
    assertCases((seed) => Murmur3_128Hex("abc", seed) !== Murmur3_128Hex("abc"), [
        [7, true],       // the seed participates
    ], "Murmur3_128 seed");
}

/* ---------------- Hasher: streaming digest over the whole table ---------------- */
{
    // d.ts: "Streaming digest over the module's whole one-shot digest table: md5|sha1|sha224|
    // sha256|sha384|sha512|sha3_224|sha3_256|sha3_384|sha3_512|keccak256|shake128|shake256|
    // blake2b|blake2s|blake3"; digestSize "reports the effective length".
    const TABLE = {
        md5: 16, sha1: 20, sha224: 28, sha256: 32, sha384: 48, sha512: 64,
        sha3_224: 28, sha3_256: 32, sha3_384: 48, sha3_512: 64, keccak256: 32,
        shake128: 32, shake256: 32, blake2b: 64, blake2s: 32, blake3: 32,
    };
    for (const name of Object.keys(TABLE)) {
        const h = new Hasher(name);
        assertEq(h.algorithm, name, "Hasher(" + name + "): algorithm getter");
        assertEq(h.digestSize, TABLE[name], "Hasher(" + name + "): digestSize (d.ts)");
        assertEq(h.digest().length, TABLE[name], "Hasher(" + name + "): digest length");
        h.close();
    }
    // streaming == one-shot, parametrically over the same table
    const ONE_SHOT = {
        md5: MD5Hex, sha1: SHA1Hex, sha224: SHA224Hex, sha256: SHA256Hex, sha384: SHA384Hex,
        sha512: SHA512Hex, sha3_224: SHA3_224Hex, sha3_256: SHA3_256Hex, sha3_384: SHA3_384Hex,
        sha3_512: SHA3_512Hex, keccak256: Keccak256Hex,
        blake2b: (d) => BLAKE2bHex(d), blake2s: (d) => BLAKE2sHex(d), blake3: (d) => BLAKE3Hex(d),
    };
    for (const name of Object.keys(ONE_SHOT)) {
        const h = new Hasher(name);
        const got = h.update("a").update("bc").digestHex();
        if (got !== ONE_SHOT[name]("abc")) throw new Error("case failed: Hasher streaming == one-shot for " + name + " — got |" + got + "|");
        n++;
        h.close();
    }
    // d.ts: update "Absorbs bytes; returns this for chaining"; accepts byte views
    assertCases(() => { const h = new Hasher("sha256"); const chained = h.update("a"); const same = chained === h; chained.update(fromUtf8("bc")); const hex = h.digestHex(); h.close(); return [same, hex]; }, [
        [undefined, [true, SHA256Hex("abc")]],
    ], "Hasher.update chaining");
    // d.ts: digest "Finalizes a copy; the stream stays usable"
    assertCases(() => { const h = new Hasher("sha256"); h.update("a"); const d1 = h.digest(); h.update("bc"); const d2 = h.digest(); h.close(); return [d1.length, eqArr(Array.from(d2), Array.from(SHA256("abc")))]; }, [
        [undefined, [32, true]],
    ], "Hasher.digest copy");
    assertCases(() => { const h = new Hasher("sha256"); h.update("abc"); const hex1 = h.digestHex(); const hex2 = h.digestHex(); h.close(); return [hex1, hex1 === hex2]; }, [
        [undefined, [SHA256Hex("abc"), true]],
    ], "Hasher.digestHex repeatable");
    // d.ts: digestInto "writes the bytes into a Uint8Array the CALLER owns (no allocation);
    // returns the byte count written. RangeError when buf.byteLength < digestSize; TypeError once
    // closed. The hasher stays usable afterwards."
    assertCases(() => { const h = new Hasher("sha256"); h.update("abc"); const out = new Uint8Array(h.digestSize); const k = h.digestInto(out); const usable = h.digestHex() === SHA256Hex("abc"); h.close(); return [k, eqArr(Array.from(out), Array.from(SHA256("abc"))), usable]; }, [
        [undefined, [32, true, true]],
    ], "Hasher.digestInto");
    assertCasesThrow(() => { const h = new Hasher("sha256"); h.update("abc"); try { h.digestInto(new Uint8Array(31)); } finally { h.close(); } }, [
        [undefined, RangeError],
    ], "Hasher.digestInto short buffer");
    assertCases(() => { const h = new Hasher("sha256"); h.update("abc"); const wide = new Uint8Array(40); const k = h.digestInto(wide.subarray(8)); const headZero = wide[0] === 0 && wide[7] === 0; h.close(); return [k, headZero, eqArr(Array.from(wide.subarray(8)), Array.from(SHA256("abc")))]; }, [
        // the bytes land at the view's own data pointer (API.md on this door)
        [undefined, [32, true, true]],
    ], "Hasher.digestInto subarray");
    // d.ts: reset "Returns the hasher to its initial state"
    assertCases(() => { const h = new Hasher("sha256"); h.update("junk"); h.reset(); h.update("abc"); const hex = h.digestHex(); h.close(); return hex; }, [
        [undefined, SHA256Hex("abc")],
    ], "Hasher.reset");
    // d.ts: "{length} sizes the extensible-output algorithms (shake128/shake256/blake3 1..2^20,
    // blake2b 1..64, blake2s 1..32); the fixed-digest algorithms reject it"
    assertCases(() => { const h = new Hasher("shake128", { length: 40 }); const size = h.digestSize; const len = h.update("abc").digestHex().length; h.close(); return [size, len]; }, [
        [undefined, [40, 80]],       // API-pinned example
    ], "Hasher shake128 length 40");
    assertCases(() => { const h = new Hasher("blake2b", { length: 32 }); const hex = h.update("abc").digestHex(); h.close(); return [hex.length, hex === BLAKE2bHex("abc", 32)]; }, [
        [undefined, [64, true]],
    ], "Hasher blake2b length 32");
    assertCases(() => { const h = new Hasher("blake3", { length: 64 }); const d = h.update("abc").digest(); h.close(); return [d.length, eqArr(Array.from(d), Array.from(BLAKE3("abc", 64)))]; }, [
        [undefined, [64, true]],
    ], "Hasher blake3 length 64");
    assertCasesThrow(() => new Hasher("sha256", { length: 16 }), [
        [undefined, RangeError],     // fixed-digest algorithms reject {length} with a RangeError (API.md)
    ], "Hasher fixed-digest length");
    assertCasesThrow((name, opts) => new Hasher(name, opts), [
        ["sha256x", undefined, TypeError],       // an unknown name throws TypeError (API.md)
        ["shake128", { length: 0 }, null],       // shake length bound 1..2^20 (d.ts)
        ["blake2b", { length: 65 }, null],
        ["blake2s", { length: 33 }, null],
    ], "Hasher constructor");
    // d.ts: close "Releases the native state early... Idempotent; every method and getter throws
    // TypeError afterwards."
    const c = new Hasher("sha256");
    c.update("x");
    c.close();
    assertCases(() => c.closed, [
        [undefined, true],           // API.md resolves the closed-getter tension: "closed reads true"
    ], "Hasher.closed after close");
    assertCases(() => { c.close(); return "idempotent"; }, [
        [undefined, "idempotent"],   // d.ts: "Idempotent"
    ], "Hasher.close idempotent");
    assertCasesThrow(() => c.update("y"), [[undefined, TypeError]], "Hasher.update after close");
    assertCasesThrow(() => c.digest(), [[undefined, TypeError]], "Hasher.digest after close");
    assertCasesThrow(() => c.digestHex(), [[undefined, TypeError]], "Hasher.digestHex after close");
    assertCasesThrow(() => c.digestInto(new Uint8Array(64)), [[undefined, TypeError]], "Hasher.digestInto after close");
    assertCasesThrow(() => c.reset(), [[undefined, TypeError]], "Hasher.reset after close");
    assertCasesThrow(() => c.algorithm, [[undefined, TypeError]], "Hasher.algorithm getter after close");
    assertCasesThrow(() => c.digestSize, [[undefined, TypeError]], "Hasher.digestSize getter after close");
}

/* ---------------- SHA3-224 / SHA3-384 depth: NIST CAVP KAT, invariants, streaming ---------------- */
{
    // Official NIST CAVP known-answer vectors, transcribed from the byte-oriented
    // SHA-3 zip on the CAVP secure-hashing page:
    //   https://csrc.nist.gov/CSRC/media/Projects/Cryptographic-Algorithm-Validation-Program/documents/sha3/sha-3bytetestvectors.zip
    // (SHA3_224ShortMsg.rsp / SHA3_384ShortMsg.rsp; each row cites its Len in bits).
    // Every hex below was cross-checked against a second independent FIPS 202
    // implementation before pinning. Len=0 rows equal the FIPS 202 values pinned
    // in the one-shot table above; the rate-boundary rows fill the sponge exactly
    // (1152 bits = the SHA3-224 rate, 832 bits = the SHA3-384 rate), so the
    // padding must spill into a second block — the multi-block case.
    const hexBytes = (s) => { const a = new Uint8Array(s.length / 2); for (let i = 0; i < a.length; i++) a[i] = parseInt(s.substr(2 * i, 2), 16); return a; };
    const MSG224 = "e65de91fdcb7606f14dbcfc94c9c94a57240a6b2c31ed410346c4dc01152655" +
        "9e44296fc988cc589de2dc713d0e82492d4991bd8c4c5e6c74c753fc093452" +
        "25e1db8d565f0ce26f5f5d9f404a28cf00bd655a5fe04edb682942d675b862" +
        "35f235965ad422ba5081a21865b8209ae81763e1c4c0cccbccdaad539cf773" +
        "413a50f5ff1267b9238f5602adc06764f775d3c";   // 144 bytes
    const MSG384 = "92c41d34bd249c182ad4e18e3b856770766f1757209675020d4c1cf7b6f7686c" +
        "8c1472678c7c412514e63eb9f5aee9f5c9d5cb8d8748ab7a5465059d9cbbb8" +
        "a56211ff32d4aaa23a23c86ead916fe254cc6b2bff7a9553df1551b531f95b" +
        "b41cbbc4acddbd372921";   // 104 bytes
    const KAT = [
        // [HexFn, message bytes, expected hex, NIST vector]
        [SHA3_224Hex, u8(), "6b4e03423667dbb73b6e15454f0eb1abd4597f9a1b078e3f5b5a6bc7", "SHA3_224ShortMsg Len=0"],
        [SHA3_224Hex, u8(0x01), "488286d9d32716e5881ea1ee51f36d3660d70f0db03b3f612ce9eda4", "SHA3_224ShortMsg Len=8"],
        [SHA3_224Hex, hexBytes("2bbb42b920b7feb4e3962a1552cc390f"), "0dfa61f6b439bf8e3a6f378fe30a4134e8b2dfb652997a2a76c2789f", "SHA3_224ShortMsg Len=128"],
        [SHA3_224Hex, hexBytes(MSG224), "26ec9df54d9afe11710772bfbeccc83d9d0439d3530777c81b8ae6a3", "SHA3_224ShortMsg Len=1152 (rate boundary)"],
        [SHA3_384Hex, u8(), "0c63a75b845e4f7d01107d852e4c2485c51a50aaaa94fc61995e71bbee983a2ac3713831264adb47fb6bd1e058d5f004", "SHA3_384ShortMsg Len=0"],
        [SHA3_384Hex, u8(0x80), "7541384852e10ff10d5fb6a7213a4a6c15ccc86d8bc1068ac04f69277142944f4ee50d91fdc56553db06b2f5039c8ab7", "SHA3_384ShortMsg Len=8"],
        [SHA3_384Hex, hexBytes("65b27f6c5578a4d5d9f6519c554c3097"), "dd734f4987fe1a71455cf9fb1ee8986882c82448827a7880fc90d2043c33b5cbc0ed58b8529e4c6bc3a7288829e0a40d", "SHA3_384ShortMsg Len=128"],
        [SHA3_384Hex, hexBytes(MSG384), "71307eec1355f73e5b726ed9efa1129086af81364e30a291f684dfade693cc4bc3d6ffcb7f3b4012a21976ff9edcab61", "SHA3_384ShortMsg Len=832 (rate boundary)"],
    ];
    for (const [fn, msg, expected, source] of KAT) {
        const got = fn(msg);
        if (got !== expected) throw new Error("case failed: NIST CAVP " + source + " — got |" + got + "| expected |" + expected + "|");
        n++;
    }
    // Invariants (d.ts): *Hex is lowercase hex of the * bytes; SHA3-224 is 28
    // bytes = 56 hex chars, SHA3-384 is 48 bytes = 96 hex chars (SIZES table above).
    assertCases((input) => [SHA3_224Hex(input).length, /^[0-9a-f]*$/.test(SHA3_224Hex(input)), HexEncode(SHA3_224(input)) === SHA3_224Hex(input)], [
        ["", [56, true, true]],
        ["abc", [56, true, true]],
        [hexBytes(MSG224), [56, true, true]],
    ], "SHA3_224Hex shape");
    assertCases((input) => [SHA3_384Hex(input).length, /^[0-9a-f]*$/.test(SHA3_384Hex(input)), HexEncode(SHA3_384(input)) === SHA3_384Hex(input)], [
        ["", [96, true, true]],
        ["abc", [96, true, true]],
        [hexBytes(MSG384), [96, true, true]],
    ], "SHA3_384Hex shape");
    // deterministic, and distinct inputs digest distinctly
    assertCases((a, b) => [SHA3_224Hex(a) === SHA3_224Hex(a), SHA3_224Hex(a) !== SHA3_224Hex(b), SHA3_384Hex(a) !== SHA3_384Hex(b)], [
        ["abc", "abd", [true, true, true]],
    ], "SHA3 determinism / distinction");
    // d.ts BytesInput: a string is its UTF-8 bytes — "€" is e2 82 ac, "\0" is 00
    assertCases((s, bytes) => [SHA3_224Hex(s) === SHA3_224Hex(bytes), SHA3_384Hex(s) === SHA3_384Hex(bytes)], [
        ["€", u8(0xe2, 0x82, 0xac), [true, true]],
        ["\0", u8(0), [true, true]],
    ], "SHA3 UTF-8/binary input parity");
    // Streaming lifecycle, SHA3 variants: streaming == one-shot byte-by-byte and
    // at arbitrary split points — including through the multi-block rate-boundary
    // KAT messages, so the streamed result lands on the NIST digest itself.
    assertCases(() => { const h = new Hasher("sha3_224"); const m = hexBytes(MSG224); for (const b of m) h.update(u8(b)); const hex = h.digestHex(); h.close(); return hex === SHA3_224Hex(m) && hex === "26ec9df54d9afe11710772bfbeccc83d9d0439d3530777c81b8ae6a3"; }, [
        [undefined, true],
    ], "Hasher sha3_224 byte-by-byte == NIST Len=1152");
    assertCases(() => { const h = new Hasher("sha3_384"); const m = hexBytes(MSG384); h.update(m.subarray(0, 1)); h.update(m.subarray(1, 103)); h.update(m.subarray(103)); const hex = h.digestHex(); h.close(); return hex === SHA3_384Hex(m) && hex === "71307eec1355f73e5b726ed9efa1129086af81364e30a291f684dfade693cc4bc3d6ffcb7f3b4012a21976ff9edcab61"; }, [
        [undefined, true],
    ], "Hasher sha3_384 split updates == NIST Len=832");
    // hash(a);hash-update(b) === hash(ab): split at "a"|"bc" lands on the FIPS 202
    // "abc" vectors pinned in the one-shot table above.
    assertCases(() => { const h1 = new Hasher("sha3_224"); const hex1 = h1.update("a").update("bc").digestHex(); h1.close(); const h2 = new Hasher("sha3_384"); const hex2 = h2.update("ab").update("c").digestHex(); h2.close(); return [hex1 === SHA3_224Hex("abc"), hex2 === SHA3_384Hex("abc")]; }, [
        [undefined, [true, true]],
    ], "Hasher sha3 update-split == FIPS 202 abc");
    // d.ts: digest "Finalizes a copy; the stream stays usable" — sha3 variant
    assertCases(() => { const h = new Hasher("sha3_384"); h.update("a"); const d1 = h.digestHex(); h.update("bc"); const d2 = h.digestHex(); h.close(); return [d1 === SHA3_384Hex("a"), d2 === SHA3_384Hex("abc")]; }, [
        [undefined, [true, true]],
    ], "Hasher sha3_384 digest copy mid-stream");
    // d.ts: update "Absorbs bytes" — byte views are BytesInput too
    assertCases(() => { const h = new Hasher("sha3_224"); h.update(fromUtf8("abc")); const hex = h.digestHex(); h.close(); return hex === SHA3_224Hex("abc"); }, [
        [undefined, true],
    ], "Hasher sha3_224 update(BytesInput)");
    // d.ts: reset "Returns the hasher to its initial state" — sha3 variant
    assertCases(() => { const h = new Hasher("sha3_224"); h.update("junk"); h.reset(); const hex = h.update("abc").digestHex(); h.close(); return hex === SHA3_224Hex("abc"); }, [
        [undefined, true],
    ], "Hasher sha3_224 reset");
    // d.ts Hasher name surface: the constructor speaks the exact strcmp list
    // "md5|sha1|sha224|sha256|sha384|sha512|sha3_224|sha3_256|sha3_384|sha3_512|
    // keccak256|shake128|shake256|blake2b|blake2s|blake3" — every documented
    // string is accepted (the TABLE loop above), a name OFF the list throws
    // TypeError, near-miss spellings included.
    const NEAR_MISSES = ["SHA3_224", "sha3-224", "Sha3_224", "sha3_384x", "SHA-384", "sha3", ""];
    for (const bad of NEAR_MISSES) {
        assertThrows(() => new Hasher(bad), "Hasher(" + JSON.stringify(bad) + ") is off the documented name list", TypeError);
    }
}

print("bb_hash: all tests passed (" + n + " assertions)");
