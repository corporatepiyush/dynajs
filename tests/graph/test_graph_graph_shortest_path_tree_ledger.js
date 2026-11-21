// flags: --std
// covers: dyna:graph.Graph.shortestPathTree
// shortestPathTree's dist and pred arrays are module buffers handed to the engine: freeing them
// must take them off the native-memory ledger (see tests/lib/ledger.js).
import { Graph } from "dyna:graph";
import { suite } from "../lib/kit.js";
import { settles } from "../lib/ledger.js";

const t = suite("structures graph shortestPathTree ledger");
t.test("shortestPathTree dist and pred leave the native ledger when collected", async (a) => {
    const N = 1 << 18, g = new Graph({ weighted: true });
    for (let i = 0; i < N; i++) g.addNode();
    for (let i = 1; i < N; i++) g.addEdge(i - 1, i, 1.5);
    await settles(a, () => g.shortestPathTree(0), N * 12);
});
await t.run();
