/* test_xxh32_spec.js -- XXH32 conformance against the xxHash reference.
 *
 * ORACLE: the published xxHash specification v0.7.3 (Cyan4973/xxHash) as
 * executed by the OFFICIAL reference C implementation through the
 * python-xxhash binding (which vendors it). Every expected digest below was
 * produced by that reference -- NEVER by this engine -- including the
 * canonical published vector XXH32("", seed 0) = 0x02CC5D05 and the four
 * seed values 0, 1, PRIME32_1 (0x9E3779B1) and 0xFFFFFFFF.
 *
 * The length matrix deliberately straddles every internal boundary of the
 * algorithm: 0..5 (the byte-tail loop), 4-byte tail steps (7, 11), the
 * 16-byte stripe boundary (15/16/17), multi-stripe (19..129), the tail
 * combinations (200, 255..257, 511..513, 1024/1025), and 4096. Inputs
 * include the empty message, all-zero and all-0xFF runs (the lanes' carry
 * chains), an alternating bit pattern, and text.
 *
 * The rotation constant of the XXH32 round is exactly what these vectors
 * bind: the round is acc = ROTL32(acc + input * PRIME32_2, 13) * PRIME32_1
 * (spec v0.7.3 sec. "XXH32 algorithm"), and any other rotation diverges on
 * the first 16-byte stripe -- which is every vector here of length >= 16.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_xxh32_spec.js */
import { XXHash32 } from "dyna:hash";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + a + ", want " + b + ")");
}

/* [seed, inputName, byteLength, expectedHex] -- from the reference, see above */
const VECTORS = [
    [0x00000000, "pat0", 0, "02CC5D05"],
    [0x00000000, "pat1", 1, "16AC78E7"],
    [0x00000000, "pat2", 2, "EA6678CB"],
    [0x00000000, "pat3", 3, "97EF5F05"],
    [0x00000000, "pat4", 4, "024E5743"],
    [0x00000000, "pat5", 5, "4B077888"],
    [0x00000000, "pat7", 7, "0C636A32"],
    [0x00000000, "pat8", 8, "C9B8A502"],
    [0x00000000, "pat11", 11, "0FCEE970"],
    [0x00000000, "pat15", 15, "1476E3A7"],
    [0x00000000, "pat16", 16, "AFD97DEB"],
    [0x00000000, "pat17", 17, "58831FCB"],
    [0x00000000, "pat19", 19, "FC3BF8A0"],
    [0x00000000, "pat31", 31, "09B935CD"],
    [0x00000000, "pat32", 32, "0DED8706"],
    [0x00000000, "pat33", 33, "A2CE0302"],
    [0x00000000, "pat47", 47, "19826B6D"],
    [0x00000000, "pat63", 63, "2EF23D21"],
    [0x00000000, "pat64", 64, "ABF55FF3"],
    [0x00000000, "pat65", 65, "DFA2A092"],
    [0x00000000, "pat80", 80, "847EF6E6"],
    [0x00000000, "pat96", 96, "04777456"],
    [0x00000000, "pat127", 127, "550EBC9C"],
    [0x00000000, "pat128", 128, "D943D624"],
    [0x00000000, "pat129", 129, "F0C46227"],
    [0x00000000, "pat200", 200, "EC11745F"],
    [0x00000000, "pat255", 255, "07FAB57E"],
    [0x00000000, "pat256", 256, "0274B24E"],
    [0x00000000, "pat257", 257, "881A2D15"],
    [0x00000000, "pat511", 511, "FB1FFF4B"],
    [0x00000000, "pat512", 512, "2D6C53C4"],
    [0x00000000, "pat513", 513, "B8FE8726"],
    [0x00000000, "pat1000", 1000, "1ED9CCCD"],
    [0x00000000, "pat1024", 1024, "10557231"],
    [0x00000000, "pat1025", 1025, "EF7C2EDA"],
    [0x00000000, "pat4096", 4096, "19CA587E"],
    [0x00000000, "zero64", 64, "56328790"],
    [0x00000000, "ff64", 64, "2B0A3D64"],
    [0x00000000, "alt64", 64, "78D9217C"],
    [0x00000000, "abc", 3, "32D153FF"],
    [0x00000000, "empty", 0, "02CC5D05"],
    [0x00000000, "fox", 43, "E85EA4DE"],
    [0x00000001, "pat0", 0, "0B2CB792"],
    [0x00000001, "pat1", 1, "9C777794"],
    [0x00000001, "pat2", 2, "186D6017"],
    [0x00000001, "pat3", 3, "C9C01622"],
    [0x00000001, "pat4", 4, "464AB43B"],
    [0x00000001, "pat5", 5, "DAE06D90"],
    [0x00000001, "pat7", 7, "325F4843"],
    [0x00000001, "pat8", 8, "1FA4E9E8"],
    [0x00000001, "pat11", 11, "7CA53BCF"],
    [0x00000001, "pat15", 15, "63412843"],
    [0x00000001, "pat16", 16, "57852632"],
    [0x00000001, "pat17", 17, "5DBC609F"],
    [0x00000001, "pat19", 19, "2F3991E8"],
    [0x00000001, "pat31", 31, "92D8AD1A"],
    [0x00000001, "pat32", 32, "9F6408E9"],
    [0x00000001, "pat33", 33, "530A9105"],
    [0x00000001, "pat47", 47, "7D398DF5"],
    [0x00000001, "pat63", 63, "25F5AFDE"],
    [0x00000001, "pat64", 64, "308A94BD"],
    [0x00000001, "pat65", 65, "E8C17574"],
    [0x00000001, "pat80", 80, "98234576"],
    [0x00000001, "pat96", 96, "CCC0CB57"],
    [0x00000001, "pat127", 127, "9834A701"],
    [0x00000001, "pat128", 128, "7A3B7C34"],
    [0x00000001, "pat129", 129, "89D185A2"],
    [0x00000001, "pat200", 200, "F00C985F"],
    [0x00000001, "pat255", 255, "5F036D8E"],
    [0x00000001, "pat256", 256, "79752268"],
    [0x00000001, "pat257", 257, "4090386F"],
    [0x00000001, "pat511", 511, "999802C5"],
    [0x00000001, "pat512", 512, "494D3269"],
    [0x00000001, "pat513", 513, "692E69D5"],
    [0x00000001, "pat1000", 1000, "192BA8E8"],
    [0x00000001, "pat1024", 1024, "00BB7401"],
    [0x00000001, "pat1025", 1025, "DE09004B"],
    [0x00000001, "pat4096", 4096, "C36AD21A"],
    [0x00000001, "zero64", 64, "4D0AC234"],
    [0x00000001, "ff64", 64, "A0114CA3"],
    [0x00000001, "alt64", 64, "C5E4874F"],
    [0x00000001, "abc", 3, "AA3DA8FF"],
    [0x00000001, "empty", 0, "0B2CB792"],
    [0x00000001, "fox", 43, "234F8471"],
    [0x9E3779B1, "pat0", 0, "36B78AE7"],
    [0x9E3779B1, "pat1", 1, "AC85C416"],
    [0x9E3779B1, "pat2", 2, "AC26F6BD"],
    [0x9E3779B1, "pat3", 3, "1BD0C0D2"],
    [0x9E3779B1, "pat4", 4, "4E9A3D56"],
    [0x9E3779B1, "pat5", 5, "810B08AA"],
    [0x9E3779B1, "pat7", 7, "EBE904A5"],
    [0x9E3779B1, "pat8", 8, "FA0E78BA"],
    [0x9E3779B1, "pat11", 11, "C73CC429"],
    [0x9E3779B1, "pat15", 15, "69EC9D9B"],
    [0x9E3779B1, "pat16", 16, "9BBBC203"],
    [0x9E3779B1, "pat17", 17, "75E5E8E5"],
    [0x9E3779B1, "pat19", 19, "CD05535D"],
    [0x9E3779B1, "pat31", 31, "E70558B2"],
    [0x9E3779B1, "pat32", 32, "9E833F1A"],
    [0x9E3779B1, "pat33", 33, "02F90374"],
    [0x9E3779B1, "pat47", 47, "9CFF9194"],
    [0x9E3779B1, "pat63", 63, "5B64BCCD"],
    [0x9E3779B1, "pat64", 64, "BAF10DC3"],
    [0x9E3779B1, "pat65", 65, "E3C0A093"],
    [0x9E3779B1, "pat80", 80, "14ED2603"],
    [0x9E3779B1, "pat96", 96, "ECC1E468"],
    [0x9E3779B1, "pat127", 127, "28781D31"],
    [0x9E3779B1, "pat128", 128, "34E6570E"],
    [0x9E3779B1, "pat129", 129, "EBDE72A0"],
    [0x9E3779B1, "pat200", 200, "B020C4F4"],
    [0x9E3779B1, "pat255", 255, "29928037"],
    [0x9E3779B1, "pat256", 256, "048056AD"],
    [0x9E3779B1, "pat257", 257, "5C472BD8"],
    [0x9E3779B1, "pat511", 511, "B440B350"],
    [0x9E3779B1, "pat512", 512, "61FEDCBF"],
    [0x9E3779B1, "pat513", 513, "2F9EA3B6"],
    [0x9E3779B1, "pat1000", 1000, "12EA2D13"],
    [0x9E3779B1, "pat1024", 1024, "EAC2748D"],
    [0x9E3779B1, "pat1025", 1025, "3D3C266F"],
    [0x9E3779B1, "pat4096", 4096, "D7718919"],
    [0x9E3779B1, "zero64", 64, "6E5B4B80"],
    [0x9E3779B1, "ff64", 64, "AA57E2D7"],
    [0x9E3779B1, "alt64", 64, "6DFA2CCE"],
    [0x9E3779B1, "abc", 3, "A1AE7709"],
    [0x9E3779B1, "empty", 0, "36B78AE7"],
    [0x9E3779B1, "fox", 43, "98C7F3BF"],
    [0xFFFFFFFF, "pat0", 0, "9061DA9D"],
    [0xFFFFFFFF, "pat1", 1, "0F641827"],
    [0xFFFFFFFF, "pat2", 2, "4D8DC198"],
    [0xFFFFFFFF, "pat3", 3, "E82849BF"],
    [0xFFFFFFFF, "pat4", 4, "6D863D8D"],
    [0xFFFFFFFF, "pat5", 5, "4D571DEC"],
    [0xFFFFFFFF, "pat7", 7, "841F4D84"],
    [0xFFFFFFFF, "pat8", 8, "1A7D2FF5"],
    [0xFFFFFFFF, "pat11", 11, "98F5DBC6"],
    [0xFFFFFFFF, "pat15", 15, "A7C70675"],
    [0xFFFFFFFF, "pat16", 16, "EE262739"],
    [0xFFFFFFFF, "pat17", 17, "D9E82E0C"],
    [0xFFFFFFFF, "pat19", 19, "2232F795"],
    [0xFFFFFFFF, "pat31", 31, "B03870AD"],
    [0xFFFFFFFF, "pat32", 32, "478D3C2F"],
    [0xFFFFFFFF, "pat33", 33, "A80EB9F4"],
    [0xFFFFFFFF, "pat47", 47, "2CDC5DA3"],
    [0xFFFFFFFF, "pat63", 63, "805FEE58"],
    [0xFFFFFFFF, "pat64", 64, "20292663"],
    [0xFFFFFFFF, "pat65", 65, "B75AA11B"],
    [0xFFFFFFFF, "pat80", 80, "73002104"],
    [0xFFFFFFFF, "pat96", 96, "22CAA559"],
    [0xFFFFFFFF, "pat127", 127, "8E420C61"],
    [0xFFFFFFFF, "pat128", 128, "99817E2E"],
    [0xFFFFFFFF, "pat129", 129, "9EB0067F"],
    [0xFFFFFFFF, "pat200", 200, "2A00AAF9"],
    [0xFFFFFFFF, "pat255", 255, "97FF8910"],
    [0xFFFFFFFF, "pat256", 256, "9AA8477C"],
    [0xFFFFFFFF, "pat257", 257, "CD3361A6"],
    [0xFFFFFFFF, "pat511", 511, "55767FA4"],
    [0xFFFFFFFF, "pat512", 512, "DA9EE52D"],
    [0xFFFFFFFF, "pat513", 513, "7556165A"],
    [0xFFFFFFFF, "pat1000", 1000, "48D44323"],
    [0xFFFFFFFF, "pat1024", 1024, "2D66C687"],
    [0xFFFFFFFF, "pat1025", 1025, "F4114285"],
    [0xFFFFFFFF, "pat4096", 4096, "995B3B40"],
    [0xFFFFFFFF, "zero64", 64, "6D18C241"],
    [0xFFFFFFFF, "ff64", 64, "084B2F39"],
    [0xFFFFFFFF, "alt64", 64, "9FDFAF9F"],
    [0xFFFFFFFF, "abc", 3, "B22B1420"],
    [0xFFFFFFFF, "empty", 0, "9061DA9D"],
    [0xFFFFFFFF, "fox", 43, "5A2A0096"],
];

const pat3 = (n) => {
    const b = new Uint8Array(n);
    for (let j = 0; j < n; j++) b[j] = (j * 37 + 3 * 53 + 11) & 0xff;
    return b;
};
const INPUTS = {
    empty: new Uint8Array(0),
    abc: new TextEncoder().encode("abc"),
    fox: new TextEncoder().encode("The quick brown fox jumps over the lazy dog"),
    zero64: new Uint8Array(64),
    ff64: new Uint8Array(64).fill(0xff),
    alt64: (() => { const b = new Uint8Array(64);
        for (let j = 0; j < 64; j += 2) { b[j] = 0xAA; b[j + 1] = 0x55; } return b; })(),
};
for (const len of [0,1,2,3,4,5,7,8,11,15,16,17,19,31,32,33,47,63,64,65,80,96,127,128,129,200,255,256,257,511,512,513,1000,1024,1025,4096])
    INPUTS["pat" + len] = pat3(len);

for (const [seed, name, len, want] of VECTORS) {
    const data = INPUTS[name];
    assert(data.length === len, name + ": fixture length matches the table");
    const got = (XXHash32(data, seed) >>> 0).toString(16).toUpperCase().padStart(8, "0");
    eq(got, want, "XXH32(" + name + ", seed 0x" + seed.toString(16) + ")");
}

/* the canonical published vector, called out on its own */
eq((XXHash32(new Uint8Array(0), 0) >>> 0).toString(16).toUpperCase().padStart(8, "0"),
   "02CC5D05", "XXH32(\"\", 0) is the published 0x02CC5D05");

/* string inputs are their UTF-8 (the BytesInput contract) */
eq((XXHash32("abc", 0) >>> 0).toString(16).toUpperCase().padStart(8, "0"),
   (XXHash32(new TextEncoder().encode("abc"), 0) >>> 0).toString(16).toUpperCase().padStart(8, "0"),
   "a string input equals its UTF-8 bytes");

/* seed boundaries */
for (const seed of [0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x9E3779B1, 0x85EBCA77])
    assert((XXHash32(INPUTS.pat64, seed) >>> 0) !== (XXHash32(INPUTS.pat64, 0) >>> 0) || seed === 0,
           "seed 0x" + seed.toString(16) + " participates in the digest");

print("test_xxh32_spec: " + (n - fails) + "/" + n + " assertions" +
      (fails ? " -- " + fails + " FAILURES" : " all passed"));
if (fails) throw new Error(fails + " failures");

