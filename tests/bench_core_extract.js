/* bench_core_extract.js -- throughput of every surface moved into src/core/.
 *
 * The core extraction (STDLIB_OOP_PLAN W1.1-W1.12) claims to be behaviour- AND
 * cost-neutral: the same algorithms, reached through one more translation unit.
 * "Neutral" is a measurement, not an assertion, so this is the instrument --
 * run it against the pre-extraction binary and this one and diff the #DATA
 * lines.
 *
 * Method, per CLAUDE.md section 3:
 *   - the timed region contains the operation and nothing else; inputs are
 *     built in an untimed setup that reruns per repetition;
 *   - an empty driving loop is calibrated and subtracted, because a JS `for`
 *     costs 4-8 ns/iteration and several of these operations are smaller;
 *   - reps are scaled so every case runs past ~25 ms (performance.now()
 *     resolves to ~1 us, so a short case reports quantised garbage);
 *   - each case is run 3x and the BEST is reported -- the minimum is the least
 *     noisy estimator for a CPU-bound loop.
 *
 * Emits `#DATA<TAB>name<TAB>ns_per_op` for machine comparison, plus a readable
 * table. Sizes are chosen to sit either side of the SIMD gate (64 bytes): a
 * 32-byte case measures per-call overhead, a 64 KiB case measures the kernel.
 */
import { SHA256Hex, SHA256, MD5Hex, SHA1Hex, CRC32, CRC32C, Hasher } from "dyna:hash";
import { HMACHex } from "dyna:crypto";
import { HexEncode, HexDecode, Base64Encode, Base64Decode,
         Base64URLEncode, Base64URLDecode, Base32Encode, Base32Decode,
         Base85Encode, Base85Decode, PutUvarint, Uvarint } from "dyna:encoding";
import { gzip, gunzip } from "dyna:compress";
import { v4, v7 } from "dyna:uuid";
import { Random } from "dyna:random";

const results = [];

/* Calibrate the bare driving loop so it can be subtracted. */
function emptyLoopNs() {
    let best = Infinity;
    for (let t = 0; t < 5; t++) {
        const N = 2000000;
        const t0 = performance.now();
        let s = 0;
        for (let i = 0; i < N; i++) s += i;
        const dt = performance.now() - t0;
        if (s === -1) print("");           /* defeat DCE without timing cost */
        best = Math.min(best, (dt * 1e6) / N);
    }
    return best;
}
const EMPTY_NS = emptyLoopNs();

/* Run `fn` reps times, 3 trials, report the best net ns/op. `reps` is chosen by
 * the caller so a trial lands past ~25 ms. */
function bench(name, reps, fn) {
    let best = Infinity;
    for (let trial = 0; trial < 3; trial++) {
        const t0 = performance.now();
        for (let i = 0; i < reps; i++) fn(i);
        const dt = performance.now() - t0;
        best = Math.min(best, (dt * 1e6) / reps);
    }
    const net = Math.max(0, best - EMPTY_NS);
    results.push([name, net]);
    print(name.padEnd(34) + net.toFixed(2).padStart(10) + " ns/op");
    print("#DATA\t" + name + "\t" + net.toFixed(4));
}

/* ---- inputs, built once outside every timed region ---- */
const buf32 = new Uint8Array(32);
const buf64k = new Uint8Array(65536);
for (let i = 0; i < buf32.length; i++) buf32[i] = (i * 37) & 0xff;
for (let i = 0; i < buf64k.length; i++) buf64k[i] = (i * 37) & 0xff;
const hex64k = HexEncode(buf64k);
const b64_64k = Base64Encode(buf64k);
const b64url64k = Base64URLEncode(buf64k);
const b32_64k = Base32Encode(buf64k);
const b85_64k = Base85Encode(buf64k);
const varintBuf = PutUvarint(0xdeadbeefn);
const text16k = "the quick brown fox jumps over the lazy dog ".repeat(380);
const gz16k = gzip(text16k);
const key = new Uint8Array(32).fill(7);

print("empty JS loop iteration: " + EMPTY_NS.toFixed(2) + " ns  (subtracted)");
print("");
print("--- dyn-hash ---");
bench("SHA256Hex/32B",      200000, () => SHA256Hex(buf32));
bench("SHA256Hex/64KiB",       400, () => SHA256Hex(buf64k));
bench("sha256raw/64KiB",       400, () => SHA256(buf64k));
bench("MD5Hex/64KiB",          800, () => MD5Hex(buf64k));
bench("SHA1Hex/64KiB",         600, () => SHA1Hex(buf64k));
bench("CRC32/64KiB",           300, () => CRC32(buf64k));
bench("CRC32C/64KiB",          300, () => CRC32C(buf64k));
bench("HMACHex-SHA256/64KiB",  400, () => HMACHex("sha256", key, buf64k));
/* the capability path: one Hasher reused, which is what the plan advertises */
{
    const h = new Hasher("sha256");
    bench("Hasher.reuse/32B",  200000, () => { h.reset(); h.update(buf32); h.digestHex(); });
}

print("");
print("--- dyn-codec ---");
bench("HexEncode/64KiB",       2000, () => HexEncode(buf64k));
bench("HexDecode/64KiB",       2000, () => HexDecode(hex64k));
bench("Base64Encode/64KiB",    2000, () => Base64Encode(buf64k));
bench("Base64Decode/64KiB",    2000, () => Base64Decode(b64_64k));
bench("Base64URLEncode/64KiB", 2000, () => Base64URLEncode(buf64k));
bench("Base64URLDecode/64KiB", 2000, () => Base64URLDecode(b64url64k));
bench("Base32Encode/64KiB",     600, () => Base32Encode(buf64k));
bench("Base32Decode/64KiB",     600, () => Base32Decode(b32_64k));
bench("Base85Encode/64KiB",     600, () => Base85Encode(buf64k));
bench("Base85Decode/64KiB",     600, () => Base85Decode(b85_64k));
bench("PutUvarint",          1000000, () => PutUvarint(0xdeadbeefn));
bench("Uvarint",             1000000, () => Uvarint(varintBuf));

print("");
print("--- dyn-compress ---");
bench("gzip/16KiB",             400, () => gzip(text16k));
bench("gunzip/16KiB",          2000, () => gunzip(gz16k));

print("");
print("--- dyn-prng / uuid ---");
bench("uuid.v4",             200000, () => v4());
bench("uuid.v7",             200000, () => v7());
{
    const r = new Random(12345);
    const fillBuf = new Uint8Array(4096);
    bench("Random.nextFloat", 2000000, () => r.nextFloat());
    bench("Random.fill/4KiB",    20000, () => r.fill(fillBuf));
}

print("");
print("cases: " + results.length);
