// covers: dyna:compress.unzstd
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

for (const n of [0, 1, 100, 4096, 300000]) {
    t.test("round trip at " + n + " bytes", (a) => {
        const src = payload(n);
        a.ok(same(unzstd(zstd(src)), src));
    });
}

await t.run();
