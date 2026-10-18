// flags: --std
// Split from tests/test_p2_batch2.js: the ml sections.
import { LinearRegression } from "dyna:ml";
import { makeTempDir, File, Path, removeAll } from "dyna:file";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " (got " + JSON.stringify(a) + " want " + JSON.stringify(b) + ")"); }
{
    const dir = makeTempDir("p2ml-");
    const p = new Path(dir.toString() + "/model.bin");
    const m = new LinearRegression();
    await m.fit([[1],[2],[3]], [2,4,6]);
    const bytes = await m.save(p);
    assert(bytes > 10, "ML save returns bytes " + bytes);
    const m2 = await LinearRegression.load(p);
    eq(JSON.stringify(await m2.predict([[4]])), JSON.stringify(await m.predict([[4]])), "ML save/load round-trip");
    const f = new File(p);
    const st = f.stat();
    const sz = st ? st.size : 0;
    assert(sz > 10, "ML save file size " + sz);
    removeAll(dir);
}
if (fails) { print("test_ml_p2_batch2: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_ml_p2_batch2 failed"); }
print("test_ml_p2_batch2: " + n + " assertions, 0 failures");
