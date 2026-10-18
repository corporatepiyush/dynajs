// flags: --std
// Split from tests/test_audit_w3_native_9.js: the crypto sections.
import { AESGCM, ChaCha20Poly1305 } from "dyna:crypto";
import { Path, makeTempDir, removeAll } from "dyna:file";
import { suite } from "../lib/kit.js";

const t = suite("test_crypto_audit_w3_native_9");
const best = (f, reps = 5) => { let b = Infinity; for (let r = 0; r < reps; r++) { const t = performance.now(); f(); b = Math.min(b, performance.now() - t); } return b; };
const T = makeTempDir("w3n9_");
// ---- D3-07: AEAD objects reuse a keyed context ----------------------------
// ORACLE: McGrew & Viega, "The Galois/Counter Mode of Operation", test cases
// 13 and 14 (AES-256, zero key, zero 96-bit IV; also in NIST's GCM vectors):
// empty plaintext -> tag 530f8afbc74536b9a963b4f1c4cb738b; sixteen zero bytes
// -> cea7403d4d606b6e074ec5d3baf39d18 || d0d1c8a799996bf0265b98b5d48ab919.
// RFC 8439 2.8.2 is the ChaCha20-Poly1305 vector. A cached context must give
// these on EVERY call, including after a failed open and with a different
// nonce in between: reusing state across calls is exactly what could break.
// CONTROL: a tampered ciphertext is refused each time.
t.test("D3-07 AEAD objects reuse a keyed context", ({ ok }) => {
    const hex = (u) => Array.from(u, (b) => b.toString(16).padStart(2, "0")).join("");
    const unhex = (s) => new Uint8Array(s.match(/../g).map((h) => parseInt(h, 16)));
    const a = new AESGCM(new Uint8Array(32)), iv = new Uint8Array(12);
    let good = 0, refused = 0;
    for (let round = 0; round < 4; round++) {
        if (hex(a.seal(iv, new Uint8Array(0))) === "530f8afbc74536b9a963b4f1c4cb738b") good++;
        if (hex(a.seal(iv, new Uint8Array(16))) === "cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919") good++;
        const ct = a.seal(new Uint8Array(12).fill(round + 1), new Uint8Array(40).fill(9), new Uint8Array([1, 2]));
        ct[3] ^= 0x10;
        try { a.open(new Uint8Array(12).fill(round + 1), ct, new Uint8Array([1, 2])); } catch (e) { if (e instanceof TypeError) refused++; }
        if (hex(a.open(iv, unhex("cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919"))) === "00".repeat(16)) good++;
    }
    ok(good === 12, "D3-07: AES-256-GCM test cases 13 and 14 hold on every reuse of one object (" + good + "/12)");
    ok(refused === 4, "control: a tampered ciphertext is refused on every round (" + refused + "/4)");

    const key = unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    const nonce = unhex("070000004041424344454647");
    const aad = unhex("50515253c0c1c2c3c4c5c6c7");
    const pt = new TextEncoder().encode("Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.");
    const c = new ChaCha20Poly1305(key);
    let cg = 0;
    for (let round = 0; round < 3; round++) {
        const out = hex(c.seal(nonce, pt, aad));
        if (out.startsWith("d31a8d34648e60db7b86afbc53ef7ec2") && out.endsWith("1ae10b594f09e26a7e902ecbd0600691")) cg++;
        c.seal(new Uint8Array(12).fill(round), pt);
    }
    ok(cg === 3, "D3-07: the RFC 8439 2.8.2 vector holds on every reuse (" + cg + "/3)");
});
t.after(() => removeAll(new Path(String(T))));
await t.run();
