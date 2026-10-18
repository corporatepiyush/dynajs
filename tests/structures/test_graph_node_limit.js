// Graph node ids are bounded at 2^26, found by the signature-driven input
// generator (tests/lib/sigfuzz.js): "nodes grow on demand", so one
// addEdge(0, 2^31-1) used to allocate and initialise two billion 16-byte node
// headers (3 s, 23 GB resident in the sweep), and 2^31 failed as an
// InternalError out-of-memory instead of a RangeError.
import { Graph } from "dyna:structures";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function err(f) { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }

for (const id of [2 ** 26, 2 ** 31 - 1, 2 ** 31, 2 ** 32 - 1]) {
    const g = new Graph();
    const t0 = performance.now();
    const m = err(() => g.addEdge(0, id));
    ok(/^RangeError: (Graph\.addEdge: node id \d+ is past the 2\^26-node limit|index out of range)$/.test(m), `addEdge(0, ${id}) is a RangeError (${m})`);
    ok(performance.now() - t0 < 200, `addEdge(0, ${id}) is refused before allocating`);
    eq(err(() => g.addEdge(id, 0)), m, `and the same for the source id ${id}`);
}
{
    const g = new Graph({ weighted: true });
    g.addEdge(0, 2 ** 16, 3);
    eq(g.nodeCount, 2 ** 16 + 1, "an id inside the limit still grows the graph (control)");
    eq(JSON.stringify(g.dijkstra(0).slice(0, 2)), "[0,null]", "and the graph still answers queries (control)");
}
{
    const g = new Graph();
    const back = Graph.deserialize(new Graph().addEdge(0, 3).serialize());
    eq(back.nodeCount, 4, "a serialized graph round-trips (control)");
}

print("test_graph_node_limit: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
