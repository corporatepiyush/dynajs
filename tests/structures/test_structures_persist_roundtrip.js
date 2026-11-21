// covers: contract dyna:structures.* -- save/load round trip of every class at every size in copy and mmap mode
// Every exported class is built at N = 128, 1K, 10K, 50K and 100K, saved, and
// loaded back in copy mode and in mmap mode. The loaded instance must be the
// same class, serialize to the same bytes (which pins content and iteration
// order), and answer the class's probe queries identically. It must also be
// live: a mutation on it saves and reloads, and the original file still loads
// unchanged afterwards, so an mmap-loaded copy never writes through. Only the
// smallest size uses the default fsync; the rest pass fsync: false to keep
// the suite off the disk-flush path.
// argv[1] is the sanitizer TIME_SCALE (the old gate's g_san_one passed 8; absent
// elsewhere). Pass it by hand when running under the sanitizer build:
//   ASAN_OPTIONS=detect_leaks=0 .obj-san/dynajs --std tests/structures/test_structures_persist_roundtrip.js 8
// (the old leg also set PEN_BUDGET_MS / DYNAJS_BUDGET_MS / TEST_BUDGET_MS to scale *
// 2000/1500/2000 for suites with wall-clock budgets; this suite reads none of them).
// Under ASan+UBSan the 50K and 100K rows alone
// overrun the leg's 150 s CPU bound, so a scale above 1 keeps N <= 10K: every
// class still round-trips at three sizes in both modes, and the release run
// keeps all five.
// Probes run before the reference serialize because some reads reorder state
// (LRU recency). The reference is reread(x) = serialize(deserialize(x)): load
// must give exactly what deserialize gives for the same bytes. RStarTree's
// bytes follow its node shape and every rebuild reshapes it, so comparing
// against x.serialize() itself would fail without any persist fault.
import * as S0 from "dyna:structures";
import { Graph as Graph0 } from "dyna:graph";
const S = { ...S0, Graph: Graph0 };
import { makeTempDir, removeAll } from "dyna:file";
import { suite } from "../lib/kit.js";
import { PERSIST, SIZES, bytesEqual } from "../lib/persist_build.js";

const SAN_SCALE = Math.max(1, parseInt((globalThis.scriptArgs || [])[1] || "1", 10) || 1);
const RUN_SIZES = SAN_SCALE > 1 ? SIZES.filter((n) => n <= 10240) : SIZES;
const t = suite("structures_persist_roundtrip");
const dir = makeTempDir("persist-rt-");
t.after(() => removeAll(dir));

t.test("every exported class has save and a static load", (a) => {
    const exported = Object.keys(S).filter((k) => typeof S[k] === "function");
    a.deep(exported.filter((k) => !PERSIST[k]), [], "classes without a persist builder");
    for (const k of exported) {
        a.eq(typeof S[k].prototype.save, "function", k + ".prototype.save");
        a.eq(typeof S[k].load, "function", k + ".load");
        a.ok(Object.prototype.hasOwnProperty.call(S[k], "load"), k + " owns load");
    }
});

for (const [name, d] of Object.entries(PERSIST)) {
    const C = S[name];
    const lopt = (mmap) => (d.arg ? { mmap, compare: d.arg } : { mmap });
    const reread = (x) => C.deserialize(x.serialize(), d.arg).serialize();
    t.test(name + " round trip at every size", (a) => {
        for (const n of RUN_SIZES) {
            const o = d.build(n);
            const probe = d.probe(o, n);
            const want = reread(o);
            const p = dir.join(name + "-" + n + ".dp");
            const sopt = n === RUN_SIZES[0] ? undefined : { fsync: false };
            const r = o.save(p, sopt);
            a.ok(r.bytes > 0 && r.written === r.bytes && r.incremental === false,
                name + " n=" + n + " save result " + JSON.stringify(r));
            for (const mmap of [false, true]) {
                const tag = name + " n=" + n + (mmap ? " mmap" : " copy");
                const L = C.load(p, lopt(mmap));
                a.must(L instanceof C, tag + " load returns a " + name);
                a.ok(bytesEqual(L.serialize(), want), tag + " serialize bytes match");
                a.deep(d.probe(L, n), probe, tag + " probe answers match");
                d.mutate(L, n);
                const after = reread(L);
                const p2 = dir.join(name + "-" + n + "-m.dp");
                L.save(p2, sopt);
                a.ok(bytesEqual(C.load(p2, lopt(mmap)).serialize(), after), tag + " mutated copy round-trips");
                a.ok(bytesEqual(C.load(p, lopt(!mmap)).serialize(), want), tag + " original file is untouched");
            }
        }
    });
}

await t.run();
