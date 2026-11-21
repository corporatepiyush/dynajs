// covers: dyna:compress.inflate
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

t.test("zlib streams from a foreign encoder decode", (a) => {
    for (const [label, z, want] of ZLIB_VECTORS) {
        a.eq(inflate(hex(z), { format: "zlib", asString: true }), want, label);
    }
});

t.test("malformed zlib framing is a SyntaxError naming the fault", (a) => {
    const good = deflate(utf8(HELLO), { format: "zlib" });
    const flip = (i, x) => { const c = good.slice(); c[i] ^= x; return c; };
    throwsLike(a, () => inflate(flip(1, 1), { format: "zlib" }), SyntaxError, /bad zlib header/, "FCHECK off by one");
    throwsLike(a, () => inflate(hex("7918"), { format: "zlib" }), SyntaxError, /bad zlib header/, "CM 9");
    throwsLike(a, () => inflate(hex("881c"), { format: "zlib" }), SyntaxError, /bad zlib header/, "CINFO 8");
    throwsLike(a, () => inflate(hex("78"), { format: "zlib" }), SyntaxError, /bad zlib header/, "one byte");
    throwsLike(a, () => inflate(hex("78bb062c0215cb00110ae5f9453929001a0b045d"), { format: "zlib" }), SyntaxError, /preset dictionaries/, "FDICT");
    throwsLike(a, () => inflate(flip(good.length - 1, 1), { format: "zlib" }), SyntaxError, /Adler-32 checksum mismatch/, "checksum");
    throwsLike(a, () => inflate(good.subarray(0, good.length - 1), { format: "zlib" }), SyntaxError, /truncated or has trailing/, "short trailer");
    const extra = new Uint8Array(good.length + 1); extra.set(good);
    throwsLike(a, () => inflate(extra, { format: "zlib" }), SyntaxError, /truncated or has trailing/, "trailing byte");
    throwsLike(a, () => inflate(deflate(utf8(HELLO)), { format: "zlib" }), SyntaxError, /bad zlib header/, "a raw stream read as zlib");
    throwsLike(a, () => inflate(good), SyntaxError, /malformed DEFLATE/, "a zlib stream read as raw");
});

await t.run();
