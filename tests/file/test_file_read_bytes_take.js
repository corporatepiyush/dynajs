// flags: --std
// covers: integration dyna:file.readBytes dyna:file.readFile -- both share the zero-copy slurp handoff
// readBytes(path) and readFile(path, {bytes: true}) hand the slurp buffer to the Uint8Array (the
// buffer is adopted and charged to the GC's malloc accounting) instead of copying it: a 16 MiB read
// was 2.58 ms against node's and bun's ~1.87 ms, the second 16 MiB memcpy being the difference. The
// slurp buffer is also sized exactly (it doubled up to a power of two, so a 17 MiB file reserved 32 MiB).
// Pinned here: bytes are exact at sizes around the inline buffer (88 B), a page and odd sizes, with data
// that does not repeat at any power-of-two period (G7); every call returns an independent buffer; and
// peak RSS plateaus over repeated 16 MiB reads, also when each buffer is only reachable from a cycle
// (reference counting frees acyclic garbage at once; cyclic garbage waits for the GC, which runs only
// when the adopted bytes are charged to its malloc accounting: uncharged, 40 reads reach ~640 MB).
// The RSS plateaus are skipped on a sanitizer build, whose quarantining allocator keeps freed buffers mapped.
// Mutants (red): the adopted length one byte short; the GC charge dropped (RSS climbs).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_read_bytes_take");
import * as std from "std";
import { Path, readFile, readBytes, writeFile, makeDir, removeAll } from "dyna:file";
import { rusage, Exec, args } from "dyna:sys";

let n = 0, fails = 0;
const sanBuild = String(Exec(args()[0], ["-e", "1"], { timeoutMs: 20000, encoding: "utf8", env: { DYNA_TEARDOWN_TRACE: "1" } }).stderr).includes("teardown: precise (build)");
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }

const dir = new Path(Path.temp(), "dj_fbtake." + (Date.now() % 10000000) + "_" + Math.random().toString(36).slice(2, 10));
makeDir(dir, { recursive: true });
const f = new Path(dir, "d.bin");

function pattern(len, seed) {
    const a = new Uint8Array(len);
    let x = (seed * 2654435761) >>> 0 || 1;
    for (let i = 0; i < len; i++) { x ^= x << 13; x >>>= 0; x ^= x >>> 17; x ^= x << 5; x >>>= 0; a[i] = x & 255; }
    return a;
}
function same(a, b) {
    if (a.length !== b.length) return "length " + a.length + " vs " + b.length;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return "byte " + i + ": " + a[i] + " vs " + b[i];
    return null;
}

await __kitS.section("exact bytes at boundary sizes, both entry points", async () => {
    const sizes = [0, 1, 87, 88, 89, 255, 256, 257, 4095, 4096, 4097, 16383, 16384, 16385, 65537, 1048573, 3 * 1048576 + 7];
    for (const sz of sizes) {
        const want = pattern(sz, sz + 11);
        writeFile(f, want);
        const a = readBytes(f), b = readFile(f, { bytes: true });
        ok(a instanceof Uint8Array && b instanceof Uint8Array, sz + ": Uint8Array results");
        ok(same(a, want) === null, sz + ": readBytes " + same(a, want));
        ok(same(b, want) === null, sz + ": readFile bytes " + same(b, want));
        ok(a.byteOffset === 0 && a.buffer.byteLength === sz, sz + ": the view covers its whole buffer (" + a.buffer.byteLength + ")");
    }
});

await __kitS.section("each read is an independent buffer; detaching one leaves the next intact", async () => {
    const want = pattern(200000, 5);
    writeFile(f, want);
    const a = readBytes(f), b = readBytes(f);
    ok(a.buffer !== b.buffer, "distinct ArrayBuffers");
    a[0] ^= 255; a[199999] ^= 255;
    ok(b[0] === want[0] && b[199999] === want[199999], "writing one does not touch the other");
    const t = a.buffer.transfer();
    ok(a.length === 0 && t.byteLength === 200000, "transfer detaches the adopted buffer");
    const c = readBytes(f);
    ok(same(c, want) === null, "a later read is intact: " + same(c, want));
});

await __kitS.section("text readFile is unchanged", async () => {
    writeFile(f, "héllo ☃ 😀 world");
    ok(readFile(f) === "héllo ☃ 😀 world", "UTF-8 text round trip");
    writeFile(f, new Uint8Array([0x61, 0xff, 0x62]));
    ok(readFile(f) === "a�b", "invalid byte is U+FFFD");
});

await __kitS.section("16 MiB reads: exact, and peak RSS plateaus over 40 repeated reads", async () => {
    const SZ = 16 * 1048576 + 3;
    const want = pattern(SZ, 77);
    writeFile(f, want);
    let last = readBytes(f);
    ok(same(last, want) === null, "16 MiB + 3 exact: " + same(last, want));
    const base = rusage().maxrss;
    for (let i = 0; i < 40; i++) last = (i & 1) ? readBytes(f) : readFile(f, { bytes: true });
    const peak = rusage().maxrss;
    const grew = (peak - base) / 1048576;
    if (sanBuild) print("SKIP RSS plateau: sanitizer build (the allocator quarantines freed buffers), grew " + grew.toFixed(1) + " MiB");
    else ok(grew < 200, "peak RSS grew " + grew.toFixed(1) + " MiB over 40 x 16 MiB reads (want a plateau, < 200)");
    ok(last.length === SZ && last[SZ - 1] === want[SZ - 1], "last read intact");
});

await __kitS.section("16 MiB reads held only by cycles: the GC sees the adopted bytes", async () => {
    const SZ = 16 * 1048576;
    writeFile(f, pattern(SZ, 9));
    let keep = null;
    const base = rusage().maxrss;
    for (let i = 0; i < 40; i++) {
        const holder = { bytes: (i & 1) ? readBytes(f) : readFile(f, { bytes: true }) };
        holder.self = holder;
        keep = holder.bytes.length;
    }
    const grew = (rusage().maxrss - base) / 1048576;
    ok(keep === SZ, "cyclic holders read the full file");
    if (sanBuild) print("SKIP RSS plateau: sanitizer build (the allocator quarantines freed buffers), grew " + grew.toFixed(1) + " MiB");
    else ok(grew < 200, "peak RSS grew " + grew.toFixed(1) + " MiB over 40 cyclic 16 MiB reads (want < 200: GC-charged)");
});

removeAll(dir);
__kitS.end();
print(`test_file_read_bytes_take: ${n - fails} of ${n} checks passed`);
std.exit(fails ? 1 : 0);
