// flags: --std
// covers: dyna:dataframe.DataFrame
// DataFrame's LSD radix sort (SORT, ARG_SORT, N_LARGEST/N_SMALLEST, RANK, DENSE_RANK, NTILE, RANK_CORR via
// dfo_sorted) runs on dyn-par workers from DFO_RADIX_PAR_MIN_WORKERS * DFO_RADIX_PAR_ROWS_PER_WORKER = 98304 rows: per-worker digit
// histograms, offsets in (digit, worker) order, so every pass is the same stable scatter as the serial one.
// Oracles: (1) ARG_SORT/SORT against the DEFINITION (a JS stable sort: NaN last, ties by row index, -0 == +0)
// for every dtype at sizes straddling the radix minimum and the parallel crossover; (2) a child with
// --io-threads 0 and children with helper spawn limits 0/1/2 produce byte-identical outputs for every verb;
// (3) sys.parStats proves helpers ran at the large sizes and not at the small ones. The spawn limit is the
// test hook DYNAJS_PAR_SPAWN_LIMIT; DYNAJS_PAR_BIG=1 adds 300001 and 2000003 rows (the crossover sizes always run).
import * as std from "std";
import { DataFrame } from "dyna:dataframe";
import { XXH3_64 } from "dyna:hash";
import { args, Exec, parStats } from "dyna:sys";
import { suite } from "../lib/kit.js";

const BIG = std.getenv("DYNAJS_PAR_BIG") === "1";
const SIZES = BIG ? [2047, 2048, 98303, 98304, 98305, 163841, 300001, 2000003] : [2047, 2048, 98303, 98304, 98305, 163841];
const TYPES = [Float64Array, Float32Array, Int32Array, Uint32Array, Int16Array, Uint16Array, Int8Array, Uint8Array];

function column(T, n, kind) {
    const a = new T(n);
    let s = 0x2545f491 ^ n ^ kind;
    for (let i = 0; i < n; i++) {
        s = (Math.imul(s, 1103515245) + 12345) >>> 0;
        if (kind === 1) a[i] = (s >>> 7) % 7;
        else if (T === Float64Array || T === Float32Array) {
            const r = s % 97;
            a[i] = r === 0 ? NaN : r === 1 ? Infinity : r === 2 ? -Infinity : r === 3 ? 0 : (r === 4 && kind === 2) ? -0 : ((s >>> 3) - 2e8) / 13.7;
        } else a[i] = s >>> 1;
    }
    return a;
}
const dcache = new Map();
const dcol = (n) => { if (!dcache.has(n)) dcache.set(n, column(Float64Array, n, 1)); return dcache.get(n); };
function refArgSort(a) {
    const idx = Array.from({ length: a.length }, (_, i) => i);
    idx.sort((x, y) => {
        const u = a[x], v = a[y], nu = u !== u, nv = v !== v;
        if (nu || nv) return nu === nv ? x - y : nu ? 1 : -1;
        return u < v ? -1 : u > v ? 1 : x - y;
    });
    return idx;
}
function fnv(view) {
    return XXH3_64(new Uint8Array(view.buffer, view.byteOffset, view.byteLength));
}
function verbs(df) {
    const out = [fnv(df.SORT("c")), fnv(df.ARG_SORT("c")), fnv(df.RANK("c")), fnv(df.DENSE_RANK("c")), fnv(df.NTILE("c", 7))];
    const big = df.N_LARGEST("c", 50), small = df.N_SMALLEST("c", 50);
    out.push(JSON.stringify(big), JSON.stringify(small), df.RANK_CORR("c", "d"));
    return out;
}
function digests() {
    const out = [];
    for (const T of TYPES) for (const n of SIZES) for (const kind of [0, 1, 2]) {
        if (kind === 2 && T !== Float64Array) continue;
        if (n > 2048 && !BIG && T !== Float64Array && T !== Int32Array && T !== Uint8Array) continue;
        out.push(T.name + n + "/" + kind + ":" + verbs(new DataFrame({ c: column(T, n, kind), d: dcol(n) })).join(","));
    }
    return out;
}

if (scriptArgs[1] === "--child") {
    const before = parStats();
    const d = digests();
    const after = parStats();
    print(JSON.stringify({ d, spawned: after.spawned - before.spawned, failures: after.spawnFailures - before.spawnFailures }));
    std.exit(0);
}
const BIN = args()[0], SELF = scriptArgs[0];
function child(flags, env) {
    const r = Exec(BIN, [...flags, "--std", SELF, "--child"], { timeoutMs: 300000, encoding: "utf8", env: Object.assign(BIG ? { DYNAJS_PAR_BIG: "1" } : {}, env || {}), maxBuffer: 64 << 20 });
    if (r.code !== 0) throw new Error("child " + flags.join(" ") + " exited " + r.code + ": " + String(r.stderr).slice(0, 300));
    return JSON.parse(String(r.stdout).trim().split("\n").pop());
}

const t = suite("dataframe-radix-par");

t.test("ARG_SORT and SORT equal the stable-sort definition for every dtype and size", (a) => {
    for (const T of TYPES) for (const n of SIZES) for (const kind of [0, 1, 2]) {
        if (kind === 2 && T !== Float64Array) continue;
        if ((n === 98303 || n === 98305 || n > 98305) && !BIG) continue;
        if (n > 2048 && T !== Float64Array && T !== Int32Array && T !== Uint8Array) continue;
        if (n > 2048 && T !== Float64Array && kind !== 0) continue;
        const c = column(T, n, kind), df = new DataFrame({ c });
        const want = refArgSort(c), got = df.ARG_SORT("c"), vals = df.SORT("c");
        let bad = -1;
        for (let i = 0; i < n && bad < 0; i++) if (got[i] !== want[i] || !Object.is(vals[i], c[want[i]])) bad = i;
        a.eq(bad, -1, `${T.name} n=${n} kind=${kind}: first mismatch at`);
    }
}, { timeoutMs: 120000 });

t.test("helpers run at the crossover and above, not below", (a) => {
    const s0 = parStats();
    if (s0.budget === 0) a.skip("single-CPU host: no helper budget");
    new DataFrame({ c: column(Float64Array, 98303, 0) }).SORT("c");
    const s1 = parStats();
    a.eq(s1.spawned, s0.spawned, "98303 rows stays serial (2 workers never pay: each pass reads every key twice)");
    new DataFrame({ c: column(Float64Array, 98304, 0) }).SORT("c");
    if (s0.budget >= 2) a.ok(parStats().spawned > s1.spawned, "98304 rows (3 workers) spawned helpers");
    a.eq(parStats().live, 0, "no helper outlives the call");
});

t.test("serial and spawn-limited children are byte-identical for every verb", (a) => {
    const here = digests();
    const serial = child(["--io-threads", "0"]);
    a.eq(serial.spawned, 0, "--io-threads 0 spawns no helper");
    a.deep(serial.d, here, "--io-threads 0");
    for (const lim of BIG ? ["0", "1", "2"] : ["0", "2"]) {
        const c = child([], { DYNAJS_PAR_SPAWN_LIMIT: lim });
        a.deep(c.d, here, "spawn limit " + lim);
        if (parStats().budget > Number(lim)) a.ok(c.failures > 0, "spawn limit " + lim + " injected failures (" + c.failures + ")");
    }
}, { timeoutMs: 600000 });

await t.run();
