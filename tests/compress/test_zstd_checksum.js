// zstd frames carry a content checksum (RFC 8878 section 3.1.1: the optional
// Content_Checksum is the low 32 bits of XXH64 over the decompressed data), so
// a corrupted body is refused instead of decoding to different bytes.
import { suite } from "../lib/kit.js";
import { zstd, unzstd } from "dyna:compress";

const t = suite("zstd_checksum");

function payload(n) {
    const b = new Uint8Array(n);
    let x = 0x9e3779b9;
    for (let i = 0; i < n; i++) { x = (x * 1664525 + 1013904223) >>> 0; b[i] = i % 7 === 0 ? x >>> 24 : 65 + (i % 23); }
    return b;
}
const same = (x, y) => x.length === y.length && x.every((v, i) => v === y[i]);

t.test("the frame header declares a content checksum", (a) => {
    const z = zstd(payload(1000));
    a.deep(Array.from(z.subarray(0, 4)), [0x28, 0xb5, 0x2f, 0xfd], "magic number");
    a.eq((z[4] >> 2) & 1, 1, "Content_Checksum_flag in the frame header descriptor");
});

for (const n of [0, 1, 100, 4096, 300000]) {
    t.test("round trip at " + n + " bytes", (a) => {
        const src = payload(n);
        a.ok(same(unzstd(zstd(src)), src));
    });
}

t.test("every level round-trips with a checksum", (a) => {
    const src = payload(5000);
    for (const level of [1, 3, 9, 19, 22]) {
        const z = zstd(src, { level });
        a.eq((z[4] >> 2) & 1, 1, "level " + level);
        a.ok(same(unzstd(z), src), "level " + level);
    }
});

t.test("checksum: false omits it and still round-trips", (a) => {
    const src = payload(5000);
    const z = zstd(src, { checksum: false });
    a.eq((z[4] >> 2) & 1, 0, "Content_Checksum_flag clear");
    a.eq(z.length, zstd(src).length - 4, "exactly the four checksum bytes shorter");
    a.ok(same(unzstd(z), src));
    a.throws(() => zstd(src, { checksum: 1 }), /must be a boolean/);
    a.throws(() => zstd(src, { chksum: true }), /unknown option/);
});

t.test("a flipped checksum byte is refused", (a) => {
    const z = zstd(payload(20000));
    z[z.length - 1] ^= 0x01;
    a.throws(() => unzstd(z));
});

t.test("no single flipped body byte decodes to different data", (a) => {
    const src = payload(3000);
    const z = zstd(src);
    let refused = 0, identical = 0, silent = 0;
    for (let i = 5; i < z.length; i++) {
        const bad = z.slice();
        bad[i] ^= 0x10;
        let out = null;
        try { out = unzstd(bad); } catch (e) { refused++; continue; }
        if (same(out, src)) identical++; else silent++;
    }
    a.eq(silent, 0, "silent corruptions");
    a.ok(refused > 0, "refused " + refused + ", identical " + identical);
});

await t.run();
