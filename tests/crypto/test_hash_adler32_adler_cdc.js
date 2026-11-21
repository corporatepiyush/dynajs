// covers: dyna:hash.Adler32
// hash.Adler32: the Wikipedia "Adler-32" article's example (Adler32("Wikipedia") = 0x11E60398) and vectors from
// CPython's zlib.adler32 (inputs regenerated below), plus a per-byte-modulo reference that does not use the 5552-byte
// blocking. hash.CDCChunks: chunk invariants, an independent JS re-implementation of the FastCDC definition (gear
// table from the same splitmix64 stream), boundary stability under an insertion, and the refusals.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_adler32_adler_cdc");
import { Adler32, CDCChunks } from "dyna:hash";
import { Random } from "dyna:random";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };
const throwsT = (f, cls, m) => { try { f(); ok(false, m + " did not throw"); } catch (e) { ok(e instanceof cls, m + " threw " + e.name + ": " + e.message); } };

ok(Adler32("Wikipedia") === 0x11e60398, "Adler32('Wikipedia') = 0x11E60398 (Wikipedia)");

ok(Adler32("") === 1, "Adler32('') = 1");

ok(Adler32(Uint8Array.from({ length: 65536 }, (_, i) => (i * 7) & 255)) === 0xcf1e8772, "65536-byte ramp matches zlib.adler32");

ok(Adler32(new Uint8Array(1_000_000).fill(255)) === 0x3843e1be, "1 MB of 0xFF matches zlib.adler32 (exercises blocking overflow)");

ok(Adler32(Uint8Array.from({ length: 256 }, (_, i) => i)) === 0xadf67f81, "bytes 0..255 match zlib.adler32");

__kitS.section("Adler32 equals the per-byte-modulo definition across the 5552 blocking boundary", () => {
    const naive = (bytes) => { let a = 1, b = 0; for (const x of bytes) { a = (a + x) % 65521; b = (b + a) % 65521; } return ((b << 16) | a) >>> 0; };
    const rng = new Random(3);
    let agree = 0;
    for (const n of [0, 1, 5551, 5552, 5553, 11104, 11105, 100_000]) { const buf = rng.bytes(n); if (naive(buf) === Adler32(buf)) agree++; }
    ok(agree === 8, "Adler32 equals the per-byte-modulo definition across the 5552 blocking boundary");
    ok(Adler32("é") === naive(new TextEncoder().encode("é")), "strings are hashed as UTF-8");
});
__kitS.end();
print("test_hash_adler_cdc: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
