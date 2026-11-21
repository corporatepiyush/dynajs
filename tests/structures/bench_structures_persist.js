// flags: --std
// save/load against the serialize + writeFile / readBytes + deserialize path
// an application would write by hand. Each class is built at N (default 100K,
// argv[1] overrides) and every arm runs in interleaved rounds (A B C A B C ...)
// so drift on the host hits all arms alike; the table shows the median.
//   save      x.save(p, {fsync:false})      vs writeFile(p, x.serialize())
//   save+sync x.save(p) (file + dir fsync)   (no hand-written equivalent)
//   inc       x.save(p, {incremental:true}) after a small change (raw classes)
//   load      C.load(p) copy and mmap       vs C.deserialize(readBytes(p))
//   first     mmap load + one probe query   (first-access latency)
//   bytes     file size                     vs serialized length
// The control row runs the hand-written save arm twice under two names; its
// spread is the noise floor any other difference must clear. The bench refuses
// to report (exit 2) when the 1-minute load average is 12 or more.
// Run: ./dynajs --std tests/structures/bench_structures_persist.js [N] [rounds] [Class,Class]
import * as std from "std";
import * as S0 from "dyna:structures";
import { Graph as Graph0 } from "dyna:graph";
const S = { ...S0, Graph: Graph0 };
import { loadAvg } from "dyna:sys";
import { makeTempDir, readBytes, removeAll, writeFile } from "dyna:file";
import { PERSIST, bytesEqual } from "../lib/persist_build.js";

const N = Number(scriptArgs[1] || 102400), ROUNDS = Number(scriptArgs[2] || 9);
const load1 = loadAvg()[0];
if (load1 >= 12) { print("host load " + load1.toFixed(1) + " >= 12: not measuring"); std.exit(2); }
const dir = makeTempDir("persist-bench-");
const ALL = ["BloomFilter", "CountMinSketch", "HyperLogLog", "BitSet", "RoaringBitmap", "SortedMap", "BTree", "OrderedMap", "ZSet", "Graph", "LRU", "Deque", "KDTree", "InvertedIndex"];
const CLASSES = scriptArgs[3] ? scriptArgs[3].split(",") : ALL;
const TWEAK = {
    BloomFilter: (o, r) => o.add("bench" + r),
    CountMinSketch: (o, r) => o.add("bench" + r, 1),
    HyperLogLog: (o, r) => { for (let i = 0; i < 50; i++) o.add("bench" + r + "-" + i); },
};

const med = (a) => { const s = a.slice().sort((x, y) => x - y); return s[s.length >> 1]; };
function timeit(fn) { const t0 = performance.now(); const r = fn(); return [performance.now() - t0, r]; }
function interleave(arms) {
    const out = {};
    for (const k of Object.keys(arms)) out[k] = [];
    for (let r = 0; r < ROUNDS; r++)
        for (const [k, fn] of Object.entries(arms)) out[k].push(timeit(() => fn(r))[0]);
    const m = {};
    for (const k of Object.keys(out)) m[k] = med(out[k]);
    return m;
}
const f = (x) => (x === undefined ? "-" : x.toFixed(2)).padStart(8);

print(`N=${N} rounds=${ROUNDS} load1=${load1.toFixed(1)} (median ms)`);
print("class".padEnd(15) + "   save  wf+ser save+sync      inc   load-c  load-mm   rd+des    first     bytes  ser-bytes");
let bad = 0;
for (const name of CLASSES) {
    const C = S[name], d = PERSIST[name], o = d.build(N);
    const p = dir.join(name + ".dp"), q = dir.join(name + ".ser"), pi = dir.join(name + "-inc.dp");
    const ser = o.serialize();
    o.save(p, { fsync: false });
    writeFile(q, ser);
    if (!bytesEqual(C.load(p, d.arg ? { compare: d.arg } : {}).serialize(), C.deserialize(readBytes(q), d.arg).serialize())) { bad++; print("MISMATCH " + name); }
    const lo = (mmap) => (d.arg ? { mmap, compare: d.arg } : { mmap });
    const arms = {
        save: () => o.save(p, { fsync: false }),
        wf: () => writeFile(q, o.serialize()),
        sync: () => o.save(p),
        loadc: () => C.load(p, lo(false)),
        loadm: () => C.load(p, lo(true)),
        rd: () => C.deserialize(readBytes(q), d.arg),
        first: () => d.probe(C.load(p, lo(true)), N),
    };
    if (TWEAK[name]) {
        o.save(pi, { fsync: false });
        arms.inc = (r) => { TWEAK[name](o, r); return o.save(pi, { incremental: true, fsync: false }); };
    }
    const m = interleave(arms);
    const bytes = readBytes(p).length;
    print(name.padEnd(15) + f(m.save) + f(m.wf) + f(m.sync) + f(m.inc) + f(m.loadc) + f(m.loadm) + f(m.rd) + f(m.first) + String(bytes).padStart(10) + String(ser.length).padStart(11));
}
{
    const o = PERSIST.SortedMap.build(N), q = dir.join("ctl.ser");
    const m = interleave({ a: () => writeFile(q, o.serialize()), b: () => writeFile(q, o.serialize()) });
    print("control A/A (SortedMap serialize+writeFile twice): " + m.a.toFixed(2) + " vs " + m.b.toFixed(2) + " ms, spread " + (100 * Math.abs(m.a - m.b) / Math.min(m.a, m.b)).toFixed(1) + "%");
}
removeAll(dir);
if (bad) std.exit(1);
