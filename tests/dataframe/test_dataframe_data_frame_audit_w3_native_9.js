// covers: dyna:dataframe.DataFrame
// Split from tests/test_audit_w3_native_9.js: the dataframe sections.
import * as df from "dyna:dataframe";
import { Path, makeTempDir, removeAll } from "dyna:file";
import { suite } from "../lib/kit.js";
import { perfCheck } from "../lib/perf.js";
import { cpuNow } from "../lib/cpuclock.js";

const t = suite("test_dataframe_audit_w3_native_9");
const best = (f, reps = 5) => { let b = Infinity; for (let r = 0; r < reps; r++) { const t = cpuNow(); f(); b = Math.min(b, cpuNow() - t); } return b; };
const T = makeTempDir("w3n9_");
// ---- M1a-06 / M1a-08: DataFrame records ----------------------------------
// ORACLE: TO_RECORDS is the row view of the columns and FROM_RECORDS is its
// inverse, so a round trip returns the same numbers and strings; the column
// set is the union of the keys in first-seen order.
// SCALING: the cost per cell cannot depend on how many columns there are.
// It did: each key was compared, as a C string, against every known column.
// CONTROL: a 20-column frame.
// F18 (flaky under docker load: 16 ns vs 82 ns): the ratio is now CPU time (cpuclock), best of 7, and the width cost
// itself is gone. 800-key records do share one shape (JS_SameShape hits, JS reads stay ~70 ns at any width: no
// dictionary mode); the growth was two O(columns^2) scans per call: the builder's duplicate-name check (strcmp
// against every earlier column, df_find_col in dfb_slot; now a name hash when > 32 columns) and the schema pass's
// "seen" scan over every known column atom (now an atom hash set). 800 columns: 23.9 -> 12.1 ns/cell (20 columns
// ~11); 2 rows x 1000 columns: 1735 -> 189 ns/column (100 columns: 333 -> 192), CPU best of 9, load 8-9.
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
        return best(() => D0.FROM_RECORDS(rs), 7) / (rows * cols);
    };
    cell(50, 20);
    const narrow = cell(4000, 20), wide = cell(100, 800);
    const perCol = (cols) => {
        const rs = [];
        for (let i = 0; i < 2; i++) { const o = {}; for (let c = 0; c < cols; c++) o["k" + c] = i + c; rs.push(o); }
        return best(() => { for (let k = 0; k < 20; k++) D0.FROM_RECORDS(rs); }, 7) / cols;
    };
    const c100 = perCol(100), c1000 = perCol(1000);
    perfCheck(ok, c1000 < c100 * 1.4, "F18: a 2-row FROM_RECORDS costs the same per column at 1000 columns as at 100 (" + (c100 * 5e4).toFixed(0) + " ns, " + (c1000 * 5e4).toFixed(0) + " ns; quadratic name checks gave 5.2x)");
    perfCheck(ok, wide < narrow * 2, "[red] M1a-08: FROM_RECORDS cost per cell does not grow with the column count (" + (narrow * 1e6).toFixed(0) + " ns at 20 columns, " + (wide * 1e6).toFixed(0) + " ns at 800)");
});
t.after(() => removeAll(new Path(String(T))));
await t.run();
