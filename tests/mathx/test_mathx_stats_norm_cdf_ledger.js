// flags: --std
// covers: dyna:mathx.stats.normCdf
// The array form of a stats distribution function returns a module buffer handed to the engine:
// freeing it must take it off the native-memory ledger (see tests/lib/ledger.js).
import { stats } from "dyna:mathx";
import { suite } from "../lib/kit.js";
import { settles } from "../lib/ledger.js";

const t = suite("mathx stats normCdf ledger");
t.test("normCdf(array) result leaves the native ledger when collected", async (a) => {
    const xs = new Float64Array(1 << 18).fill(0.25);
    await settles(a, () => stats.normCdf(xs), xs.length * 8);
});
await t.run();
