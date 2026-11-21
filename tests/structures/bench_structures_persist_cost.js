// flags: --std
// P13: CPU and memory cost of save/load for every dyna:structures class and a dyna:docdb Database (DocDB row: persist/Database.load), against a
// JSON.stringify+writeFile / readFile+JSON.parse baseline over the same elements
// (o.toArray() or [...o]; "-" when the class exposes neither). Every cell runs in a
// fresh child process so peak RSS is per operation:
//   save   C.load(file) first, then o.save(p,{fsync:false}); RSS = maxrss growth over the
//          loaded state (the transient a save adds), ms = calling-thread CPU, best of R
//   load   C.load(p) in a fresh process; RSS = maxrss growth over the empty process
//   jsave/jload the same for the JSON baseline (its parse is the data, not a live structure)
// CPU is sys.threadCpuNow (not wall; the DocDB row uses process CPU, since persist and load run off the JS thread) so a loaded host shifts the numbers less; the
// 1-minute load average is printed. Sizes 1K/10K/100K/1M; 1M is skipped for a class
// whose 100K build took more than 3 s. Output: a TSV table on stdout.
// Run: ./dynajs --std tests/structures/bench_structures_persist_cost.js [Class,Class] [sizes] [R]
import * as std from "std";
import * as S0 from "dyna:structures";
import { Graph as Graph0 } from "dyna:graph";
const S = { ...S0, Graph: Graph0 };
import { args, Spawn, loadAvg, threadCpuNow as threadCpu, cpuUsage, rusage } from "dyna:sys";
import { makeTempDir, readFile, readBytes, remove, removeAll, writeFile, Path } from "dyna:file";
import { PERSIST as PERSIST0 } from "../lib/persist_build.js";
import { Database } from "dyna:docdb";

const DDB = {
    build(n) { const db = new Database("p13"); const c = db.collection("c"); const B = 10000; for (let i = 0; i < n; i += B) { const docs = []; for (let j = i; j < Math.min(n, i + B); j++) docs.push({ _id: j, name: "name-" + j, v: j * 1.5, tags: ["a", "b" + (j % 7)], nested: { k: j % 101, ok: (j & 1) === 0 } }); c.insertMany(docs); } return db; },
};
const PERSIST = Object.assign({ DocDB: DDB }, PERSIST0);
const isDB = (name) => name === "DocDB";
const dbSave = (o, p) => o.persist(String(p), { fsync: false });
const dbLoad = (p) => Database.load(String(p));
const dbPlain = (o) => o.collection("c").find({});

const plain = (o) => {
    const tries = [() => o.toArray(), () => [...o.entries()], () => [...o], () => [...o.keys()], () => { const c = o.exportCSR(); return [Array.from(c.offsets), Array.from(c.edges), c.weights ? Array.from(c.weights) : null]; }];
    for (const t of tries) { try { const a = t(); if (Array.isArray(a)) return a; } catch (e) { } }
    return null;
};
const jsonable = (a) => JSON.stringify(a, (k, v) => (typeof v === "bigint" ? String(v) : v));

if (scriptArgs[1] === "--child") {
    const [, , op, name, nS, dir, rS] = scriptArgs;
    const n = Number(nS), R = Number(rS), C = S[name], d = PERSIST[name];
    const p = new Path(dir + "/" + name + "-" + n + ".dp"), q = new Path(dir + "/" + name + "-" + n + ".json"), ps = new Path(dir + "/" + name + "-" + n + ".dp.s"), qs = new Path(dir + "/" + name + "-" + n + ".json.s");
    const lo = d.arg ? { compare: d.arg } : {};
    const out = {};
    if (op === "prep") {
        const t0 = Date.now();
        const o = d.build(n);
        out.buildMs = Date.now() - t0;
        if (isDB(name)) await dbSave(o, p); else o.save(p, { fsync: false });
        const a = isDB(name) ? dbPlain(o) : plain(o);
        if (a) writeFile(q, jsonable(a));
        out.bytes = readBytes(p).length;
        out.jbytes = a ? readBytes(q).length : 0;
    } else {
        const procCpu = () => { const u = cpuUsage(); return (u.user + u.system) * 1000; };
        const threadCpuNow = isDB(name) ? procCpu : threadCpu;
        let o = null;
        if (op === "save") { o = isDB(name) ? await dbLoad(p) : C.load(p, lo); std.gc(); }
        if (op === "jsave") { o = JSON.parse(readFile(q)); std.gc(); }
        const r0 = rusage().maxrss;
        let best = Infinity;
        for (let i = 0; i < R; i++) {
            const t0 = threadCpuNow();
            let keep;
            if (op === "save") { if (isDB(name)) await dbSave(o, ps); else o.save(ps, { fsync: false }); }
            else if (op === "load") keep = isDB(name) ? await dbLoad(p) : C.load(p, lo);
            else if (op === "jsave") writeFile(qs, jsonable(o));
            else if (op === "jload") keep = JSON.parse(readFile(q));
            const dt = threadCpuNow() - t0;
            if (dt < best) best = dt;
            if (i === 0) out.rss = rusage().maxrss - r0;
            keep = null;
            std.gc();
        }
        out.ms = best;
    }
    print(JSON.stringify(out));
    std.exit(0);
}

const CLASSES = scriptArgs[1] && scriptArgs[1] !== "all" ? scriptArgs[1].split(",") : Object.keys(PERSIST).sort();
const SIZES = scriptArgs[2] ? scriptArgs[2].split(",").map(Number) : [1000, 10000, 100000, 1000000];
const R = Number(scriptArgs[3] || 3);
const BIN = args()[0];
const dir = String(makeTempDir("p13-"));
async function child(op, name, n) {
    const sp = new Spawn(BIN, ["--std", scriptArgs[0], "--child", op, name, String(n), dir, String(R)], { stdin: "ignore", timeoutMs: 300000 });
    const chunks = []; const b = new Uint8Array(65536);
    for (;;) { const k = await sp.stdout.read(b); if (k <= 0) break; chunks.push(new TextDecoder().decode(b.subarray(0, k))); }
    const r = await sp.wait(); sp.close();
    const txt = chunks.join("").trim().split("\n").pop();
    if (r.code !== 0) return { err: "rc" + r.code + (r.signal || "") };
    try { return JSON.parse(txt); } catch (e) { return { err: "parse" }; }
}
const kb = (x) => (x === undefined ? "-" : (x / 1024).toFixed(0));
const ms = (x) => (x === undefined ? "-" : x.toFixed(2));
print("# load1=" + loadAvg()[0].toFixed(1) + " R=" + R + " (CPU ms best of R; RSS KiB peak growth; bytes)");
print(["class", "n", "save_ms", "load_ms", "jsave_ms", "jload_ms", "save_rss_kb", "load_rss_kb", "jload_rss_kb", "bytes", "json_bytes", "build_ms"].join("\t"));
for (const name of CLASSES) {
    let lastBuild = 0;
    for (const n of SIZES) {
        if (n >= 1000000 && lastBuild > 3000) { print([name, n, "skipped (100K build " + lastBuild + " ms)"].join("\t")); continue; }
        const pr = await child("prep", name, n);
        if (pr.err) { print([name, n, "prep " + pr.err].join("\t")); break; }
        lastBuild = pr.buildMs;
        const sv = await child("save", name, n), ld = await child("load", name, n);
        const js = pr.jbytes ? await child("jsave", name, n) : {}, jl = pr.jbytes ? await child("jload", name, n) : {};
        print([name, n, ms(sv.ms), ms(ld.ms), ms(js.ms), ms(jl.ms), kb(sv.rss), kb(ld.rss), kb(jl.rss), pr.bytes, pr.jbytes || "-", pr.buildMs].join("\t"));
        for (const f of [".dp", ".dp.s", ".json", ".json.s"]) { try { remove(new Path(dir + "/" + name + "-" + n + f)); } catch (e) { } }
    }
}
removeAll(new Path(dir));
