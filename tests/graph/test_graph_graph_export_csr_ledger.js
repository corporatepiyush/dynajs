// flags: --std
// covers: dyna:graph.Graph.exportCSR
// exportCSR's weights array is a module buffer handed to the engine: freeing the result must take
// it off the native-memory ledger (see tests/lib/ledger.js for the failure this pins).
import { Graph } from "dyna:graph";
import { suite } from "../lib/kit.js";
import { settles } from "../lib/ledger.js";

const t = suite("structures graph exportCSR ledger");
t.test("exportCSR weights leave the native ledger when collected", async (a) => {
    const N = 1 << 18, g = new Graph({ weighted: true });
    for (let i = 0; i < N; i++) g.addNode();
    for (let i = 1; i < N; i++) g.addEdge(i - 1, i, 1.5);
    await settles(a, () => g.exportCSR(), (N - 1) * 8);
});
await t.run();
