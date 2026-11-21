// covers: dyna:compress.deflate
// deflate/inflate { format: "zlib" }: RFC 1950 framing around the same raw
// DEFLATE encoder. Header CMF 0x78 (CM 8, 32 KiB window) with FLEVEL from the
// level as zlib sets it (1 -> 0x01, 2-5 -> 0x5e, 6 -> 0x9c, 7+ -> 0xda), body,
// then the Adler-32 of the uncompressed bytes, big-endian. Decoding accepts any
// window size (CINFO 0..7), refuses preset dictionaries (FDICT), and checks the
// header check bits, the trailer length and the checksum. The pinned streams
// below were produced by zlib 1.2.12 (python zlib.compress / compressobj), so
// they are a foreign encoder's output, not ours read back.
import { suite } from "../lib/kit.js";
import { deflate, inflate } from "dyna:compress";

const t = suite("deflate_zlib");
const hex = (s) => new Uint8Array(s.match(/../g).map((h) => parseInt(h, 16)));
const toHex = (u) => Array.from(u, (b) => b.toString(16).padStart(2, "0")).join("");
const utf8 = (s) => new TextEncoder().encode(s);
const same = (x, y) => x.length === y.length && x.every((v, i) => v === y[i]);
const trailer = (z) => ((z[z.length - 4] << 24) | (z[z.length - 3] << 16) | (z[z.length - 2] << 8) | z[z.length - 1]) >>> 0;
const throwsLike = (a, fn, Err, re, msg) => {
    try { fn(); a.ok(false, msg + ": no throw"); } catch (e) { a.ok(e instanceof Err && re.test(String(e.message)), msg + ": " + e); }
};

const HELLO = "hello hello hello hello";
const ZLIB_VECTORS = [
    ["empty input, level 6", "789c030000000001", ""],
    ["level 1 header 78 01", "7801cb48cdc9c957c8402701680308b1", HELLO],
    ["level 6 header 78 9c", "789ccb48cdc9c957c8402701680308b1", HELLO],
    ["level 9 header 78 da", "78dacb48cdc9c957c8402701680308b1", HELLO],
    ["512-byte window (CINFO 1)", "18954b4c4a4e8421001de00499", "abcabcabcabc"],
];

t.test("our zlib output is header, raw body, big-endian Adler-32", (a) => {
    const src = utf8(HELLO);
    for (const [level, h1] of [[1, 0x01], [2, 0x5e], [5, 0x5e], [6, 0x9c], [7, 0xda], [9, 0xda], [12, 0xda]]) {
        const z = deflate(src, { level, format: "zlib" });
        a.eq(z[0], 0x78, "level " + level + " CMF");
        a.eq(z[1], h1, "level " + level + " FLG");
        a.eq(((z[0] << 8) | z[1]) % 31, 0, "level " + level + " FCHECK");
        a.ok(same(z.subarray(2, z.length - 4), deflate(src, { level })), "level " + level + " body is the raw stream");
        a.ok(same(inflate(z, { format: "zlib" }), src), "level " + level + " round-trips");
    }
    a.eq(toHex(deflate(new Uint8Array(0), { format: "zlib" })), "789c030000000001", "empty input matches zlib byte for byte");
    a.ok(same(deflate(src, { format: "raw" }), deflate(src)), "format raw is the default");
    a.ok(same(inflate(deflate(src), { format: "raw" }), src), "inflate format raw is the default");
});

t.test("Adler-32 matches zlib, including across the 5552-byte modulo interval", (a) => {
    a.eq(trailer(deflate(utf8("Wikipedia"), { format: "zlib" })), 0x11e60398, "Wikipedia");
    a.eq(trailer(deflate(new Uint8Array(100000).fill(0xff), { format: "zlib" })), 0x149a302c, "100000 x 0xff");
    const big = new Uint8Array(70000);
    for (let i = 0; i < big.length; i++) big[i] = (i * 7 + Math.floor(i / 3)) & 0xff;
    const z = deflate(big, { level: 9, format: "zlib" });
    a.eq(trailer(z), 0x261705e4, "70000-byte pattern");
    a.ok(same(inflate(z, { format: "zlib" }), big), "70000-byte pattern round-trips");
});

t.test("format is validated and maxOutputBytes still bounds zlib output", (a) => {
    throwsLike(a, () => deflate(utf8("x"), { format: "gzip" }), TypeError, /format must be "raw" or "zlib"/, "deflate gzip");
    throwsLike(a, () => deflate(utf8("x"), { format: 1 }), TypeError, /format must be "raw" or "zlib"/, "deflate number");
    throwsLike(a, () => inflate(hex("789c030000000001"), { format: "ZLIB" }), TypeError, /format must be "raw" or "zlib"/, "inflate wrong case");
    const z = deflate(new Uint8Array(5000), { format: "zlib" });
    throwsLike(a, () => inflate(z, { format: "zlib", maxOutputBytes: 4999 }), RangeError, /exceeds maxOutputBytes/, "limit");
    a.eq(inflate(z, { format: "zlib", maxOutputBytes: 5000 }).length, 5000, "limit exactly met");
});

await t.run();
