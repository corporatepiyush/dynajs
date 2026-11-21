// Graph analytics kernels (dyna:structures Graph / graph) on a seeded planted-partition graph, best of N wall-clock runs.
// The same graph is written as an edge list for bench/bench_graph_analytics_nx.py, which times networkx on it, so the
// two tables compare like for like. Usage: dynajs bench/bench_graph_analytics.js [nodes] [avgDegree] [runs] [edgeFile]
import { Graph } from "dyna:structures";
import { writeFile, Path } from "dyna:file";

const N = Number(scriptArgs[1] ?? 20000), DEG = Number(scriptArgs[2] ?? 16), RUNS = Number(scriptArgs[3] ?? 5);
const OUT = scriptArgs[4];
let s = 0x2545f491n;
const next = () => { s ^= s << 13n; s &= (1n << 64n) - 1n; s ^= s >> 7n; s ^= s << 17n; s &= (1n << 64n) - 1n; return Number(s % 4294967296n) / 4294967296; };
const BLOCK = 200, g = new Graph({ directed: false }), lines = [];
for (let i = 0; i < N; i++) g.addNode();
for (let u = 0; u < N; u++)
    for (let k = 0; k < DEG / 2; k++) {
        const v = next() < 0.9 ? Math.min(N - 1, Math.floor(u / BLOCK) * BLOCK + Math.floor(next() * BLOCK)) : Math.floor(next() * N);
        if (v === u) continue;
        g.addEdge(u, v);
        lines.push(u + " " + v);
    }
if (OUT) writeFile(new Path(OUT), lines.join("\n") + "\n");
async function best(f) {
    let b = Infinity;
    for (let r = 0; r < RUNS; r++) { const t = performance.now(); await f(); b = Math.min(b, performance.now() - t); }
    return b;
}
const rows = [
    ["pageRank (tol 1e-6)", () => g.pageRank()],
    ["triangles", () => g.triangles()],
    ["clustering", () => g.clustering()],
    ["coreNumbers", () => g.coreNumbers()],
    ["louvain", () => g.louvain()],
    ["labelPropagation", () => g.labelPropagation()],
    ["betweenness (64 sources)", () => g.betweenness({ samples: 64 })],
    ["weaklyConnectedComponents", () => g.weaklyConnectedComponents()],
    ["connectedComponents (sync)", () => g.connectedComponents()],
];
print(`nodes ${N}, edges ${g.edgeCount}, best of ${RUNS}`);
for (const [name, f] of rows) print(name.padEnd(30) + (await best(f)).toFixed(2).padStart(10) + " ms");
