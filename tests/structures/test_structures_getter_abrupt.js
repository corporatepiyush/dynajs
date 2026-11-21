// flags: --std
// timeout: 240
// covers: contract dyna:structures.CSRGraph -- D49: edge element getters propagate
// st1s D49: the CSRGraph constructor read [src, dst, weight] of each edge and passed the values to the number conversion
// without a JS_IsException check, so a throwing element getter was converted instead of propagated. The structures
// decoders (btree, interval tree, min-max heap, multimap, range map, table, patricia, ordered trees), dsx_replay and the
// static installer now check their reads too; those read dense arrays they built themselves, so only allocation failure
// reaches them. Oracle: each hostile edge throws exactly the sentinel; GC inside edge getters keeps the graph; a
// --timeout-ms interrupt inside an edge getter ends a catch-all loop.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_getter_abrupt");
import { Exec, args } from "dyna:sys";
import * as std from "std";
const SELF = "tests/structures/test_structures_getter_abrupt.js";
const SENT = new Error("test_structures_getter_abrupt sentinel");
const thrower = () => { throw SENT; };
const spin = () => { let i = 0; for (;;) i++; };
const run = (f) => { try { return { v: f() }; } catch (e) { return { e }; } };
const with_ = (base, k, get = thrower) => { const x = { ...base }; delete x[k]; Object.defineProperty(x, k, { get, enumerable: true, configurable: true }); return x; };
const gcw = (base) => { const x = Array.isArray(base) ? [] : {}; for (const k of Object.keys(base)) { const v = base[k]; Object.defineProperty(x, k, { get() { std.gc(); return v; }, enumerable: true, configurable: true }); } return x; };
const getArr = (vals, i, get = thrower) => { const a = vals.slice(); Object.defineProperty(a, i, { get, enumerable: true, configurable: true }); return a; };
const lenProxy = (vals, len = thrower) => new Proxy(vals.slice(), { get(t, k, r) { return k === "length" ? len() : Reflect.get(t, k, r); } });
const __NAME = "test_structures_getter_abrupt";
import { CSRGraph } from "dyna:structures";
const PINNED = [["CSRGraph", () => { const g = new CSRGraph(4, [[0, 1], [1, 2, 2.5], [2, 3]], { directed: true }); return g.nodes + ":" + g.edgeCount + ":" + Array.from(g.neighbors(1)).join(","); }]];
const INTERRUPTS = { interrupt: () => new CSRGraph(2, [getArr([0, 1], 0, spin)]) };

if (args().includes("--child")) {
    const mode = args()[args().indexOf("--child") + 1];
    if (INTERRUPTS[mode]) {
        for (;;) { try { INTERRUPTS[mode](); } catch (e) {} }
    }
    std.exit(0);
}
let n = 0, fails = 0;
const ok = (c, w) => { n++; if (!c) { fails++; print("  FAIL  " + w); } };
const desc = (r) => r.e !== undefined ? "threw " + (r.e === SENT ? "SENT" : String(r.e)) : "returned " + String(JSON.stringify(r.v)).slice(0, 160);
const sent = (w, f) => { const r = run(f); ok(r.e === SENT, w + ": " + desc(r)); ok(run(() => JSON.parse("[1]")).v?.[0] === 1, w + ": nothing left pending"); };

await __kitS.section("CSRGraph edge elements propagate every getter", () => {
    for (const i of [0, 1, 2]) sent("CSRGraph edge element " + i, () => new CSRGraph(2, [getArr([0, 1, 3], i)]));
    sent("CSRGraph edges Proxy length", () => new CSRGraph(2, lenProxy([[0, 1]])));
    const r = run(() => { const g = new CSRGraph(3, [gcw([0, 1, 2]), gcw([1, 2])], { directed: true }); return g.edgeCount + ":" + Array.from(g.neighbors(0)).join(","); });
    ok(r.v === "2:1", "gc in edge getters keeps the graph: " + desc(r));
});

await __kitS.section("pinned controls and interrupts that must end a catch-all loop", () => {
    for (const mode of Object.keys(INTERRUPTS)) {
        const t0 = Date.now();
        const i = Exec(args()[0], ["--std", "--timeout-ms", "300", SELF, "--child", mode], { timeoutMs: 20000 });
        ok(i.code !== 0 && !i.timedOut && /interrupt/i.test(i.stderr + i.stdout), mode + " child ends: rc " + i.code + " after " + (Date.now() - t0) + " ms " + i.stderr.slice(-200));
    }
});
if (fails) throw new Error(fails + " of " + n + " checks failed");
print(__NAME + ": " + n + " checks passed");
