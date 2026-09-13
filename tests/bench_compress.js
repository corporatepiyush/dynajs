/* bench_compress.js -- the compression tiers, and the W0-G crossover for
 * `class Compressor` (W8.6).
 *
 * Two questions, and they are separate:
 *
 *   1. WHAT DOES EACH TIER COST? Ratio and MB/s for gzip, lz4 and lz4hc on
 *      three corpora: log lines, JSON, and -- the adversarial one that stays
 *      here permanently -- ALREADY-COMPRESSED bytes, where compression must
 *      not expand the input and every cycle spent trying is wasted.
 *   2. IS THE CAPABILITY WORTH IT? A Compressor hoists the match-finder
 *      scratch (64 KiB of hash heads plus 4 bytes per input byte) out of the
 *      per-call path. That is a real cost removed, unlike `Codec`'s alphabet
 *      lookup -- but the plan's rule is that a capability ships only past a
 *      measured crossover, so here it is, at N = 1, 2, 3, 5, 10, 100, 1000.
 *      The N=1 row is the bypass-never-fires case and is kept forever.
 *
 * Emits `#DATA<TAB>kind<TAB>case<TAB>a<TAB>b<TAB>c`.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/bench_compress.js */

import { gzip, gunzip, lz4Compress, lz4Decompress, Compressor } from "dyna:compress";

const enc = new TextEncoder();

/* ---- corpora ---- */
function logLines(n) {
    const lvls = ["INFO", "WARN", "ERROR", "DEBUG"];
    let s = "";
    for (let i = 0; i < n; i++)
        s += "2026-07-27T10:" + String(i % 60).padStart(2, "0") + ":00Z " +
             lvls[i % 4] + " [worker-" + (i % 8) + "] request id=" + i +
             " path=/api/v1/resource/" + (i % 500) + " status=200 dur=" +
             (i % 97) + "ms\n";
    return enc.encode(s);
}
function jsonPayload(n) {
    const rows = [];
    for (let i = 0; i < n; i++)
        rows.push({ jsonrpc: "2.0", id: i, method: "subscribe",
                    params: { channel: "trades", symbol: "SYM" + (i % 40), depth: 10 } });
    return enc.encode(JSON.stringify(rows));
}
const CORPORA = {
    logs: logLines(4000),
    json: jsonPayload(3000),
    /* THE ADVERSARIAL CASE: gzip output is already compressed, so nothing can
     * be found and every tier must decline to expand it. Kept permanently --
     * CLAUDE.md section 4 requires the direction where the bypass never fires. */
    precompressed: gzip(logLines(4000)),
};

function timeBest(fn, minMs) {
    const floor = minMs === undefined ? 30 : minMs;
    let reps = 1;
    for (;;) {
        const t0 = performance.now();
        for (let i = 0; i < reps; i++) fn();
        const dt = performance.now() - t0;
        if (dt >= floor || reps >= (1 << 22)) {
            let best = Infinity;
            for (let t = 0; t < 3; t++) {
                const s = performance.now();
                for (let i = 0; i < reps; i++) fn();
                const d = (performance.now() - s) / reps;
                if (d < best) best = d;
            }
            return best;                       /* ms per call */
        }
        reps = Math.max(reps * 2, Math.ceil(reps * floor / Math.max(dt, 1e-4)));
    }
}

print("=== 1. the tiers: ratio and throughput ===");
print("corpus         tier      bytes ->   bytes   ratio    comp MB/s   decomp MB/s");
for (const [name, data] of Object.entries(CORPORA)) {
    const tiers = [
        ["gzip", (d) => gzip(d), (p) => gunzip(p)],
        ["lz4", (d) => lz4Compress(d), (p) => lz4Decompress(p)],
        ["lz4hc", (d) => lz4Compress(d, { level: 12 }), (p) => lz4Decompress(p)],
    ];
    for (const [tier, comp, decomp] of tiers) {
        const packed = comp(data);
        const cms = timeBest(() => comp(data));
        const dms = timeBest(() => decomp(packed));
        const mb = data.length / (1 << 20);
        const ratio = packed.length / data.length;
        print(`${name.padEnd(14)} ${tier.padEnd(8)} ${String(data.length).padStart(8)} -> ` +
              `${String(packed.length).padStart(7)}  ${ratio.toFixed(4)}  ` +
              `${(mb / (cms / 1000)).toFixed(1).padStart(10)}  ` +
              `${(mb / (dms / 1000)).toFixed(1).padStart(12)}`);
        print(`#DATA\ttier\t${name}/${tier}\t${data.length}\t${packed.length}\t${ratio.toFixed(4)}`);
        if (name === "precompressed" && packed.length > data.length * 1.02)
            print(`  !! ${tier} EXPANDED already-compressed input by ` +
                  `${((packed.length / data.length - 1) * 100).toFixed(1)}%`);
    }
}
print("");

print("=== 2. the W0-G crossover for class Compressor ===");
print("A capability pays iff the per-call work its constructor hoists exceeds");
print("(K-1) x 20.3 ns of extra call ceremony. Here the hoisted work is the");
print("match-finder scratch: a 64 KiB table plus 4 bytes per input byte,");
print("malloc'd and initialised on every free-function call.");
print("");
const NS = [1, 2, 3, 5, 10, 100, 1000];

/* The crossover corpora are RECORD-sized, not file-sized, and that is the
 * point: at 400 KB a call takes ~700 us and the fixed cost is invisible under
 * the measurement noise, which is exactly what the first run of this bench
 * produced -- a "crossover" that bounced between 3 and 1000 across a grid
 * because the free-function column varied 2x run to run. The workload a reused
 * Compressor exists for is a stream of records. */
const CROSS = {
    "record(91B)": enc.encode(JSON.stringify({ jsonrpc: "2.0", id: 7,
        method: "subscribe", params: { channel: "trades", symbol: "SYM7" } })),
    "chunk(4KB)": logLines(40),
    "page(64KB)": logLines(640),
};
for (const [name, data] of Object.entries(CROSS)) {
    const ratios = [];
    for (const N of NS) {
        const free = timeBest(() => { for (let i = 0; i < N; i++) lz4Compress(data); }) / N;
        const cap = timeBest(() => {
            const c = new Compressor({ algo: "lz4" });
            for (let i = 0; i < N; i++) c.compress(data);
            c.close();
        }) / N;
        const r = cap / free;
        ratios.push(r);
        print(`Compressor/${name.padEnd(12)} N=${String(N).padStart(4)}  ` +
              `free ${(free * 1e6).toFixed(0).padStart(8)} ns  cap ${(cap * 1e6).toFixed(0).padStart(8)} ns  ` +
              `ratio ${r.toFixed(3)}` + (r < 1 ? "  WIN" : ""));
        print(`#DATA\tcrossover\t${name}\t${N}\t${(free * 1e6).toFixed(1)}\t${(cap * 1e6).toFixed(1)}\t${r.toFixed(4)}`);
    }
    let cross = null;
    for (let i = 0; i < NS.length; i++) if (ratios[i] < 1) { cross = NS[i]; break; }
    print(`>>> Compressor on ${name}: crossover at N=` +
          (cross === null ? ">1000 (DOES NOT PAY)" : cross));
    print("");
}

/* The dictionary is the other half of W8.4, and its win is a RATIO win on
 * short templated payloads where LZ77 has no window to work with. */
{
    const dict = enc.encode('{"jsonrpc":"2.0","id":,"method":"subscribe",' +
                            '"params":{"channel":"trades","symbol":"SYM","depth":10}}');
    const msg = enc.encode('{"jsonrpc":"2.0","id":42,"method":"subscribe",' +
                           '"params":{"channel":"trades","symbol":"SYM7","depth":10}}');
    const withD = new Compressor({ algo: "lz4", dict });
    const plain = new Compressor({ algo: "lz4" });
    const a = plain.compress(msg), b = withD.compress(msg);
    print("=== 3. the dictionary, on the payload shape it exists for ===");
    print(`one ${msg.length}-byte JSON-RPC frame: plain ${a.length} B, ` +
          `with a ${dict.length}-byte dictionary ${b.length} B ` +
          `(${(b.length / a.length).toFixed(3)}x)`);
    print(`#DATA\tdict\tjsonrpc\t${msg.length}\t${a.length}\t${b.length}`);
    print(`gzip of the same frame: ${gzip(msg).length} B -- DEFLATE has no ` +
          `window either, and its Huffman header costs more than it saves`);
}
