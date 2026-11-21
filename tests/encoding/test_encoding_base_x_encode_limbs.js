// covers: c:src/modules/encoding/encoding-basex.c
// BaseX/Base58 encode and decode run the quadratic base conversion on 32-bit
// limbs: encode keeps limbs in base B^k (the largest power of the alphabet size
// that fits in 32 bits, k = 5 for base 58) and folds in three input bytes per
// step; decode keeps base-2^32 limbs and folds in k digits per step. That cuts
// the inner-loop trips by about 3k (15x for base 58) and replaces per-byte
// division by a runtime base with per-limb division. Modperf round 2026-11-15:
// 256-byte Base58 encode+decode 0.144 ms -> 0.0104 ms. Oracle: BigInt base
// conversion in JS with the usual leading-zero rule (each leading 0x00 byte is
// one alphabet[0] digit and back).
import { suite } from "../lib/kit.js";
import * as e from "dyna:encoding";

const t = suite("test_encoding_base_x_encode_limbs");

const B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
function refEnc(b, A) {
    let z = 0;
    while (z < b.length && b[z] === 0) z++;
    let v = 0n;
    for (let i = z; i < b.length; i++) v = v * 256n + BigInt(b[i]);
    const base = BigInt(A.length);
    let s = "";
    while (v > 0n) {
        s = A[Number(v % base)] + s;
        v /= base;
    }
    return A[0].repeat(z) + s;
}
function refDec(s, A) {
    let z = 0;
    while (z < s.length && s[z] === A[0]) z++;
    let v = 0n;
    for (let i = z; i < s.length; i++) {
        const d = A.indexOf(s[i]);
        if (d < 0) return "bad";
        v = v * BigInt(A.length) + BigInt(d);
    }
    const out = [];
    while (v > 0n) {
        out.unshift(Number(v & 255n));
        v >>= 8n;
    }
    return new Array(z).fill(0).concat(out).join(",");
}

let seed = 77;
const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff), seed);
const ALPHAS = [B58, "01", "012", "0123456789", "0123456789abcdef", "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/",
    Array.from({ length: 94 }, (_, i) => String.fromCharCode(33 + i)).join("")];

t.test("Base58 encode/decode equal BigInt conversion at lengths 0..300", ({ eq }) => {
    for (let n = 0; n <= 300; n++)
        for (const kind of [0, 1, 2]) {
            const b = new Uint8Array(n);
            for (let i = 0; i < n; i++) b[i] = kind === 0 ? rnd() & 255 : kind === 1 ? (i < 4 ? 0 : rnd() & 255) : 255;
            const s = e.Base58Encode(b);
            eq(s, refEnc(b, B58), "enc n=" + n + " kind " + kind);
            eq(Array.from(e.Base58Decode(s)).join(","), refDec(s, B58), "dec n=" + n + " kind " + kind);
        }
});

t.test("BaseX over alphabets of 2..94 symbols, and random digit strings", ({ eq }) => {
    for (const A of ALPHAS)
        for (let n = 0; n <= 70; n++) {
            const b = new Uint8Array(n);
            for (let i = 0; i < n; i++) b[i] = n % 5 === 0 && i < 2 ? 0 : rnd() & 255;
            const s = e.BaseXEncode(b, A);
            eq(s, refEnc(b, A), "enc base " + A.length + " n=" + n);
            eq(Array.from(e.BaseXDecode(s, A)).join(","), refDec(s, A), "dec base " + A.length + " n=" + n);
            let r = "";
            for (let i = 0; i < n; i++) r += A[rnd() % A.length];
            eq(Array.from(e.BaseXDecode(r, A)).join(","), refDec(r, A), "rand digits base " + A.length + " n=" + n);
        }
});

t.test("bad digits are rejected and checks still round-trip", ({ eq, throws }) => {
    throws(() => e.Base58Decode("1110"), undefined, "0 is not base58");
    throws(() => e.BaseXDecode("0129", "012"), undefined, "9 not in alphabet");
    for (let n = 0; n < 40; n++) {
        const b = new Uint8Array(n).map(() => rnd() & 255);
        eq(Array.from(e.Base58CheckDecode(e.Base58CheckEncode(b))).join(","), Array.from(b).join(","), "check n=" + n);
    }
});

await t.run();
