/* bench_jsonpath.js -- what a compiled JSONPath costs, and when it pays.
 *
 * A compiled capability is NOT automatically a win: the honest figures are the
 * construction cost, the per-call cost, and the number of uses at which it
 * beats writing the walk by hand. Both are published, including the row where
 * the hand-written walk wins.
 *
 * The CONTROL is the hand-written walk itself: it does not use the module, so
 * any change to the module must leave it flat.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/bench_jsonpath.js
 */
import { JSONPath } from "dyna:encoding";

const REPS = 2000;
let sink = 0;

function bench(name, reps, fn) {
    fn();                                    /* warm */
    const t0 = performance.now();
    for (let i = 0; i < reps; i++) sink += fn();
    const ms = performance.now() - t0;
    print("  " + name.padEnd(46) + (ms * 1e6 / reps).toFixed(0).padStart(8) + " ns/op");
    return ms * 1e6 / reps;
}

/* A document of the shape this is actually used on: a response body. */
function makeDoc(nrows) {
    const rows = [];
    for (let i = 0; i < nrows; i++)
        rows.push({ id: i, name: "row" + i, price: (i % 40) + 0.5,
                    tags: ["a", "b"], meta: { active: i % 3 === 0, score: i % 7 } });
    return { data: { rows: rows }, page: { next: null } };
}
const small = makeDoc(10), big = makeDoc(1000);

print("construction (the cost a compiled capability adds up front)");
bench("new JSONPath('$.data.rows[*].name')", REPS,
      () => new JSONPath("$.data.rows[*].name").hasOwnProperty ? 1 : 0);
bench("new JSONPath (a filter with && and a $ operand)", REPS,
      () => new JSONPath("$.data.rows[?@.price<20 && @.meta.score>$.page.next]") ? 1 : 0);

print("\nper call, 10 rows");
{
    const q = new JSONPath("$.data.rows[*].name");
    const hand = (d) => d.data.rows.map((r) => r.name);
    const a = bench("compiled  $.data.rows[*].name", REPS, () => q.all(small).length);
    const b = bench("CONTROL   hand-written .map                  ", REPS,
                    () => hand(small).length);
    print("  ratio " + (a / b).toFixed(2) + "x  (>1 means the hand-written walk wins)");
}

print("\nper call, 1000 rows");
{
    const q = new JSONPath("$.data.rows[*].name");
    const hand = (d) => d.data.rows.map((r) => r.name);
    const a = bench("compiled  $.data.rows[*].name", 200, () => q.all(big).length);
    const b = bench("CONTROL   hand-written .map                  ", 200,
                    () => hand(big).length);
    print("  ratio " + (a / b).toFixed(2) + "x");
}

print("\nfilter, 1000 rows");
{
    const q = new JSONPath("$.data.rows[?@.price<20].id");
    const hand = (d) => d.data.rows.filter((r) => r.price < 20).map((r) => r.id);
    const a = bench("compiled  [?@.price<20].id", 200, () => q.all(big).length);
    const b = bench("CONTROL   hand-written .filter().map()       ", 200,
                    () => hand(big).length);
    print("  ratio " + (a / b).toFixed(2) + "x");
}

print("\ndescendant segment, 1000 rows (no hand-written equivalent is one line)");
bench("$..price", 100, () => new JSONPath("$..price").all(big).length);
{
    const q = new JSONPath("$..price");
    bench("$..price, compiled once                       ", 100, () => q.all(big).length);
}

print("\nTHE CROSSOVER: compile once, then use N times (10 rows)");
{
    const expr = "$.data.rows[?@.price<20].id";
    const hand = (d) => d.data.rows.filter((r) => r.price < 20).map((r) => r.id);
    for (const uses of [1, 2, 5, 10, 50]) {
        const t0 = performance.now();
        for (let r = 0; r < 500; r++) {
            const q = new JSONPath(expr);
            for (let u = 0; u < uses; u++) sink += q.all(small).length;
        }
        const cm = performance.now() - t0;
        const t1 = performance.now();
        for (let r = 0; r < 500; r++)
            for (let u = 0; u < uses; u++) sink += hand(small).length;
        const ch = performance.now() - t1;
        print("  " + String(uses).padStart(3) + " use(s): compiled "
              + (cm * 1e6 / 500).toFixed(0).padStart(7) + " ns   hand "
              + (ch * 1e6 / 500).toFixed(0).padStart(7) + " ns   ratio "
              + (cm / ch).toFixed(2) + "x");
    }
}

print("\nsink " + (sink === 0 ? "ZERO -- the loops were optimised away" : "ok"));
