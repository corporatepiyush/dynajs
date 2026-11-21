// flags: --std
// covers: dyna:graph.Graph.pageRank
// pageRank's Float64Array (like every analytics result) is a module buffer handed to the engine:
// freeing it must take it off the native-memory ledger (see tests/lib/ledger.js).
import { Graph } from "dyna:graph";
import { suite } from "../lib/kit.js";
import { settles } from "../lib/ledger.js";

const t = suite("structures graph pageRank ledger");
t.test("pageRank result leaves the native ledger when collected", async (a) => {
    const N = 1 << 18, g = new Graph();
    for (let i = 0; i < N; i++) g.addNode();
    for (let i = 1; i < N; i++) g.addEdge(i - 1, i);
    await settles(a, () => g.pageRank(), N * 8);
});
await t.run();
