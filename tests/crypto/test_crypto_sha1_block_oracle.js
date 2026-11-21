// covers: c:src/core/dyn-hash.c
// On arm64 with the SHA extensions the SHA-1 block function runs on the SHA1C/P/M/H/SU0/SU1
// instructions (the scalar 80-round loop was 54-72% of SHA1, HMAC-SHA1 / TOTP and UUID v5);
// other targets, and builds with DYN_SHA1_NO_HW or DYN_SHA256_NO_HW, keep the scalar loop. The
// oracle is a JS SHA-1 (FIPS 180-4): every length 0..300 and lengths around many block
// boundaries, in one call and fed in random pieces, plus the FIPS / RFC 3174 answers, HMAC-SHA1
// per RFC 2202 and UUID v5 per RFC 9562 must agree.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_sha1_block_oracle");
import * as H from "dyna:hash";
import * as CR from "dyna:crypto";
import * as UU from "dyna:uuid";

let pass = 0, fail = 0;
const eq = (a, b, m) => { if (a === b) pass++; else { fail++; if (fail < 30) print("FAIL " + m + ": got " + a + ", want " + b); } };
const hex = (u) => Array.from(u, (b) => (b < 16 ? "0" : "") + b.toString(16)).join("");
let seed = 17;
const bytes = (n) => { const b = new Uint8Array(n); for (let i = 0; i < n; i++) { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; b[i] = seed >>> 24; } return b; };
function sha1(msg) {
    const ml = msg.length, n = ((ml + 8) >> 6) + 1, w = new Uint32Array(80);
    const p = new Uint8Array(n * 64);
    p.set(msg);
    p[ml] = 0x80;
    const bits = ml * 8;
    p[n * 64 - 4] = bits >>> 24; p[n * 64 - 3] = bits >>> 16; p[n * 64 - 2] = bits >>> 8; p[n * 64 - 1] = bits;
    p[n * 64 - 5] = Math.floor(ml / 0x20000000);
    let h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476, h4 = 0xc3d2e1f0;
    const rl = (x, c) => (x << c) | (x >>> (32 - c));
    for (let b = 0; b < n; b++) {
        for (let i = 0; i < 16; i++) w[i] = (p[b * 64 + i * 4] << 24) | (p[b * 64 + i * 4 + 1] << 16) | (p[b * 64 + i * 4 + 2] << 8) | p[b * 64 + i * 4 + 3];
        for (let i = 16; i < 80; i++) w[i] = rl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        let a = h0, bb = h1, c = h2, d = h3, e = h4;
        for (let i = 0; i < 80; i++) {
            const f = i < 20 ? (bb & c) | (~bb & d) : i < 40 || i >= 60 ? bb ^ c ^ d : (bb & c) | (bb & d) | (c & d);
            const k = i < 20 ? 0x5a827999 : i < 40 ? 0x6ed9eba1 : i < 60 ? 0x8f1bbcdc : 0xca62c1d6;
            const t = (rl(a, 5) + f + e + k + w[i]) | 0;
            e = d; d = c; c = rl(bb, 30); bb = a; a = t;
        }
        h0 = (h0 + a) | 0; h1 = (h1 + bb) | 0; h2 = (h2 + c) | 0; h3 = (h3 + d) | 0; h4 = (h4 + e) | 0;
    }
    return [h0, h1, h2, h3, h4].map((x) => (x >>> 0).toString(16).padStart(8, "0")).join("");
}

__kitS.section("known answers", () => {
    const te = new TextEncoder();
    eq(H.SHA1Hex(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709", "SHA1 of empty");
    eq(H.SHA1Hex("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA1 of abc");
    eq(H.SHA1Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"), "84983e441c3bd26ebaae4aa1f95129e5e54670f1", "SHA1 of the 448-bit message");
    eq(H.SHA1Hex("a".repeat(1000000)), "34aa973cd4c4daa4f61eeb2bdbad27316534016f", "SHA1 of a million a");
    eq(CR.HMACHex("sha1", new Uint8Array(20).fill(0x0b), te.encode("Hi There")), "b617318655057264e28bc0b6fb378c8ef146be00", "HMAC-SHA1 RFC 2202 case 1");
    eq(UU.v5(UU.NAMESPACE_DNS, "www.example.com"), "2ed6657d-e927-568b-95e1-2665a8aea6a2", "UUID v5 RFC 9562 example");
});

__kitS.section("every length against the JS oracle", () => {
    const lens = [];
    for (let i = 0; i <= 300; i++) lens.push(i);
    for (const b of [512, 1024, 4096, 65536]) for (let d = -9; d <= 9; d++) lens.push(b + d);
    for (const len of lens) {
        const m = bytes(len);
        const want = sha1(m);
        eq(H.SHA1Hex(m), want, "SHA1 of " + len + " bytes");
        const h = new H.Hasher("sha1");
        let o = 0;
        while (o < len) { const k = Math.min(len - o, 1 + (bytes(1)[0] % 97)); h.update(m.subarray(o, o + k)); o += k; }
        eq(hex(h.digest()), want, "SHA1 of " + len + " bytes fed in pieces");
        h.close();
    }
});

__kitS.end();
print(`test_crypto_sha1_block_oracle: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failed");
