// flags: --std
// covers: dyna:csv.index
// A typed column of an index (CsvIndex.column) is a module buffer handed to the engine: freeing it must take it off the
// native-memory ledger (see tests/lib/ledger.js).
import { index } from "dyna:csv";
import { suite } from "../lib/kit.js";
import { settles } from "../lib/ledger.js";

const t = suite("csv CsvIndex.column ledger");
const R = 1 << 18, rows = ["a,b"];
for (let i = 0; i < R; i++) rows.push(i + "," + i * 0.5);
const ix = index(rows.join("\n") + "\n");
t.test("an f64 column leaves the native ledger when collected", async (a) => {
    await settles(a, () => ix.column("b", "f64"), R * 8);
});
t.test("a dict column's codes leave the native ledger when collected", async (a) => {
    await settles(a, () => ix.column("a", "dict"), R * 4);
});
await t.run();
