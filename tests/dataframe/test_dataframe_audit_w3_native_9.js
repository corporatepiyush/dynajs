// flags: --std
// Split from tests/test_audit_w3_native_9.js: the dataframe sections.
import * as df from "dyna:dataframe";
import { Path, makeTempDir, removeAll } from "dyna:file";
import { suite } from "../lib/kit.js";

const t = suite("test_dataframe_audit_w3_native_9");
const best = (f, reps = 5) => { let b = Infinity; for (let r = 0; r < reps; r++) { const t = performance.now(); f(); b = Math.min(b, performance.now() - t); } return b; };
const T = makeTempDir("w3n9_");
// ---- M1a-06 / M1a-08: DataFrame records ----------------------------------
// ORACLE: TO_RECORDS is the row view of the columns and FROM_RECORDS is its
// inverse, so a round trip returns the same numbers and strings; the column
// set is the union of the keys in first-seen order.
// SCALING: the cost per cell cannot depend on how many columns there are.
// It did: each key was compared, as a C string, against every known column.
// CONTROL: a 20-column frame.
t.test("M1a-06 M1a-08 DataFrame records", ({ ok }) => {
    const D0 = new df.DataFrame({ a: new Float64Array(1) });
    const recs = [{ x: 1, name: "a" }, { x: 2.5, name: "b", extra: 9 }, { name: "c", x: -0.25 }];
    const D = D0.FROM_RECORDS(recs);
    const back = D.TO_RECORDS();
    ok(back.length === 3 && back[0].x === 1 && back[1].x === 2.5 && back[2].x === -0.25, "FROM_RECORDS/TO_RECORDS round-trips numbers");
    ok(back.map((r) => r.name).join() === "a,b,c", "and strings (" + back.map((r) => r.name).join() + ")");
    ok(Object.keys(back[0]).join() === "x,name,extra", "columns are the union of keys in first-seen order (" + Object.keys(back[0]).join() + ")");
    ok(Number.isNaN(back[0].extra) && back[1].extra === 9, "a key missing from a row is NaN in that row");
    const csv = D.TO_CSV().split("\n");
    ok(csv[0] === "x,name,extra" && csv[2] === "2.5,b,9", "TO_CSV writes the same cells (" + csv[2] + ")");

    const cell = (rows, cols) => {
        const rs = [];
        for (let i = 0; i < rows; i++) { const o = {}; for (let c = 0; c < cols; c++) o["k" + c] = i + c; rs.push(o); }
        return best(() => D0.FROM_RECORDS(rs), 3) / (rows * cols);
    };
    cell(50, 20);
    const narrow = cell(4000, 20), wide = cell(100, 800);
    ok(wide < narrow * 4, "[red] M1a-08: FROM_RECORDS cost per cell does not grow with the column count (" + (narrow * 1e6).toFixed(0) + " ns at 20 columns, " + (wide * 1e6).toFixed(0) + " ns at 800)");
});
t.after(() => removeAll(new Path(String(T))));
await t.run();
