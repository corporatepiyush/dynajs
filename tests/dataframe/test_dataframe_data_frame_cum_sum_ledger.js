// flags: --std
// covers: dyna:dataframe.DataFrame
// CUM_SUM (like every DataFrame Float64Array result) is a module buffer handed to the engine:
// freeing it must take it off the native-memory ledger (see tests/lib/ledger.js).
import { DataFrame } from "dyna:dataframe";
import { suite } from "../lib/kit.js";
import { settles } from "../lib/ledger.js";

const t = suite("dataframe CUM_SUM ledger");
t.test("CUM_SUM result leaves the native ledger when collected", async (a) => {
    const n = 1 << 18, df = new DataFrame({ a: new Float64Array(n).fill(1) });
    await settles(a, () => df.CUM_SUM("a"), n * 8);
});
await t.run();
