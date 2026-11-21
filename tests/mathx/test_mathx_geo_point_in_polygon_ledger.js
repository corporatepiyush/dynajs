// flags: --std
// covers: dyna:mathx.geo.pointInPolygon
// The array form of pointInPolygon returns a module buffer handed to the engine: freeing it must
// take it off the native-memory ledger (see tests/lib/ledger.js).
import { geo } from "dyna:mathx";
import { suite } from "../lib/kit.js";
import { settles } from "../lib/ledger.js";

const t = suite("mathx geo pointInPolygon ledger");
t.test("pointInPolygon(arrays) mask leaves the native ledger when collected", async (a) => {
    const n = 1 << 21, lat = new Float64Array(n).fill(0.5), lon = new Float64Array(n).fill(0.5);
    await settles(a, () => geo.pointInPolygon(lat, lon, [0, 0, 1, 1], [0, 1, 1, 0]), n, 64 * 2 ** 20);
});
await t.run();
