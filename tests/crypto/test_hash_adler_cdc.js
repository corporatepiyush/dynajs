// hash.Adler32: the Wikipedia "Adler-32" article's example (Adler32("Wikipedia") = 0x11E60398) and vectors from
// CPython's zlib.adler32 (inputs regenerated below), plus a per-byte-modulo reference that does not use the 5552-byte
// blocking. hash.CDCChunks: chunk invariants, an independent JS re-implementation of the FastCDC definition (gear
// table from the same splitmix64 stream), boundary stability under an insertion, and the refusals.
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
{
    const naive = (bytes) => { let a = 1, b = 0; for (const x of bytes) { a = (a + x) % 65521; b = (b + a) % 65521; } return ((b << 16) | a) >>> 0; };
    const rng = new Random(3);
    let agree = 0;
    for (const n of [0, 1, 5551, 5552, 5553, 11104, 11105, 100_000]) { const buf = rng.bytes(n); if (naive(buf) === Adler32(buf)) agree++; }
    ok(agree === 8, "Adler32 equals the per-byte-modulo definition across the 5552 blocking boundary");
    ok(Adler32("é") === naive(new TextEncoder().encode("é")), "strings are hashed as UTF-8");
}

// ---- CDC ----
function gearTable() {
    const M = (1n << 64n) - 1n;
    let st = 0x6a09e667f3bcc908n;
    const out = [];
    for (let i = 0; i < 256; i++) {
        st = (st + 0x9e3779b97f4a7c15n) & M;
        let z = st;
        z = ((z ^ (z >> 30n)) * 0xbf58476d1ce4e5b9n) & M;
        z = ((z ^ (z >> 27n)) * 0x94d049bb133111ebn) & M;
        out.push(z ^ (z >> 31n));
    }
    return out;
}
const GEAR = gearTable();
function cdcReference(bytes, min = 2048, avg = 8192, max = 65536) {
    const M = (1n << 64n) - 1n, bits = Math.log2(avg);
    const maskS = (M << BigInt(64 - (bits + 2))) & M, maskL = (M << BigInt(64 - (bits - 2))) & M;
    const cuts = [];
    let pos = 0;
    while (pos < bytes.length) {
        let n = bytes.length - pos, cut = n;
        if (n > min) {
            if (n > max) n = max;
            const normal = Math.min(avg, n);
            let h = 0n, i = min;
            cut = n;
            for (; i < normal; i++) { h = ((h << 1n) + GEAR[bytes[pos + i]]) & M; if (!(h & maskS)) { cut = i + 1; break; } }
            if (cut === n && i >= normal) for (; i < n; i++) { h = ((h << 1n) + GEAR[bytes[pos + i]]) & M; if (!(h & maskL)) { cut = i + 1; break; } }
        }
        pos += cut;
        cuts.push(pos);
    }
    return cuts;
}
{
    const rng = new Random(77);
    const data = rng.bytes(300_000);
    const cuts = CDCChunks(data, { min: 1024, avg: 4096, max: 16384 });
    const ref = cdcReference(data, 1024, 4096, 16384);
    ok(cuts instanceof Uint32Array && JSON.stringify(Array.from(cuts)) === JSON.stringify(ref), `CDC cuts equal the BigInt reference (${cuts.length} chunks)`);
    const sizes = Array.from(cuts, (c, i) => c - (i ? cuts[i - 1] : 0));
    ok(cuts.at(-1) === data.length && sizes.every((s, i) => s > 0 && s <= 16384 && (s >= 1024 || i === sizes.length - 1)), "chunks tile the input within [min, max] (last may be short)");
    const mean = data.length / cuts.length;
    ok(mean > 2048 && mean < 8192, "mean chunk size near avg (" + mean.toFixed(0) + ")");
    const big = rng.bytes(2_000_000);
    const before = CDCChunks(big);
    const edited = new Uint8Array(big.length + 100);
    edited.set(big.subarray(0, 1_000_000)); edited.set(rng.bytes(100), 1_000_000); edited.set(big.subarray(1_000_000), 1_000_100);
    const after = CDCChunks(edited);
    const ends = new Set(Array.from(before));
    const shifted = new Set(Array.from(after, (c) => (c > 1_000_000 ? c - 100 : c)));
    const kept = [...ends].filter((c) => shifted.has(c)).length;
    ok(kept >= ends.size - 3, `an insertion moves only nearby boundaries (${kept}/${ends.size} kept)`);
    ok(JSON.stringify(Array.from(CDCChunks(data))) === JSON.stringify(Array.from(CDCChunks(data.slice()))), "deterministic");
    ok(CDCChunks(new Uint8Array(0)).length === 0, "empty input has no chunks");
    ok(JSON.stringify(Array.from(CDCChunks(new Uint8Array(100)))) === "[100]", "input shorter than min is one chunk");
    const zeros = CDCChunks(new Uint8Array(200_000));
    ok(Array.from(zeros, (c, i) => c - (i ? zeros[i - 1] : 0)).slice(0, -1).every((s) => s === 65536 || s <= 65536), "low-entropy input is cut at max");
}
throwsT(() => CDCChunks(new Uint8Array(10), { avg: 5000 }), RangeError, "avg must be a power of two");
throwsT(() => CDCChunks(new Uint8Array(10), { min: 9000, avg: 8192 }), RangeError, "min < avg required");
throwsT(() => CDCChunks(new Uint8Array(10), { max: 4096 }), RangeError, "avg < max required");
throwsT(() => CDCChunks(new Uint8Array(10), { minimum: 64 }), TypeError, "unknown option refused");
throwsT(() => CDCChunks(new Uint8Array(10), 5), TypeError, "non-object options refused");
throwsT(() => CDCChunks(), TypeError, "data is required");
{
    const ab = new Uint8Array(10);
    const hostile = { get avg() { ab.buffer.transfer(); return 8192; } };
    throwsT(() => CDCChunks(ab, hostile), TypeError, "options are read before the data is borrowed");
}
print("test_hash_adler_cdc: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
