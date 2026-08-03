/* A realistic object-literal-heavy workload: turning rows into records and
 * processing them. This is the shape of most data-handling JS -- an ORM
 * hydrating rows, an API layer mapping a payload, an event pipeline -- where
 * the object literal is a means to an end rather than the thing being measured.
 *
 * It exists because the repo's other benchmarks do NOT build object literals in
 * a hot loop: JetStream's Octane set uses `new Ctor()` (which already gets
 * constructor pre-sizing), b_objects uses classes, and b_json builds objects
 * inside the native JSON parser rather than through OP_object. A microbenchmark
 * of literal construction alone proves the mechanism works but not that anyone
 * benefits; this is the check that someone does.
 *
 * Deliberately NOT tuned to the optimisation: the records are consumed (sorted,
 * grouped, reduced) so construction is a realistic fraction of the work rather
 * than 100% of it.
 *
 * Reading the result: case [1] hydrate improves ~1.77x and the full pipeline
 * ~12%, but case [2] "project" does NOT move -- and that is the useful signal,
 * not a defect. Its values are `r.id` / `r.amount`, i.e. OP_get_field results,
 * not leaf ops, so the analysis skips it. Extending pre-sizing to non-leaf value
 * expressions needs stack-height tracking through the literal to tell an inner
 * literal's define_fields from the outer's (`{a:{b:1}}` emits the inner ones
 * first), which the leaf restriction sidesteps. Case [2] is the standing
 * measurement of what that follow-up would be worth.
 */
function bench(name, f) {
    for (let i = 0; i < 3; i++) f();
    let best = Infinity;
    for (let r = 0; r < 5; r++) {
        const t0 = performance.now(); const v = f(); const t1 = performance.now();
        if (t1 - t0 < best) best = t1 - t0;
    }
    console.log("  " + name.padEnd(38) + best.toFixed(2).padStart(8) + " ms");
    return best;
}

const N = 200000;

/* raw columnar input, as it would arrive from a driver or a CSV reader */
const ids = new Int32Array(N);
const amounts = new Float64Array(N);
const names = [], cities = [];
const CITY = ["London", "Paris", "Tokyo", "Lagos", "Lima", "Oslo"];
for (let i = 0; i < N; i++) {
    ids[i] = i;
    amounts[i] = (i % 977) * 1.5;
    names.push("user" + (i % 5000));
    cities.push(CITY[i % CITY.length]);
}

/* 1. hydrate: rows -> record objects (5 fields, all leaf reads of let-bound
      loop state -- the exact shape that used to miss the optimisation) */
function hydrate() {
    const out = new Array(N);
    for (let i = 0; i < N; i++) {
        const id = ids[i], amount = amounts[i], name = names[i], city = cities[i];
        out[i] = { id, name, city, amount, active: true };
    }
    return out;
}
const t1 = bench("hydrate 200k records (5 fields)", hydrate);

const rows = hydrate();

/* 2. map to a projected record -- the other ubiquitous literal shape */
const t2 = bench("project to {id,total} 200k", () => {
    const out = new Array(rows.length);
    for (let i = 0; i < rows.length; i++) {
        const r = rows[i];
        out[i] = { id: r.id, total: r.amount, city: r.city };
    }
    return out;
});

/* 3. group into buckets, allocating an accumulator literal per new key */
const t3 = bench("group by city + reduce", () => {
    const acc = Object.create(null);
    for (let i = 0; i < rows.length; i++) {
        const r = rows[i];
        let g = acc[r.city];
        if (g === undefined) { g = { count: 0, sum: 0, min: Infinity, max: -Infinity }; acc[r.city] = g; }
        g.count++;
        g.sum += r.amount;
        if (r.amount < g.min) g.min = r.amount;
        if (r.amount > g.max) g.max = r.amount;
    }
    return acc;
});

/* 4. full pipeline: hydrate -> filter -> project -> sort -> summarise */
const t4 = bench("full pipeline", () => {
    const recs = hydrate();
    const kept = [];
    for (let i = 0; i < recs.length; i++)
        if (recs[i].amount > 500) kept.push({ id: recs[i].id, amount: recs[i].amount });
    kept.sort((a, b) => a.amount - b.amount);
    let s = 0;
    for (let i = 0; i < kept.length; i++) s += kept[i].amount;
    return s;
});

/* 5. JSON round-trip of literal-built records (stringify walks the shape) */
const t5 = bench("stringify 20k records", () => {
    const small = [];
    for (let i = 0; i < 20000; i++)
        small.push({ id: ids[i], name: names[i], city: cities[i], amount: amounts[i] });
    return JSON.stringify(small).length;
});

console.log("\n  total " + (t1 + t2 + t3 + t4 + t5).toFixed(2) + " ms");
