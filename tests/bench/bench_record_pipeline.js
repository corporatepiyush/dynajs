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

const t2 = bench("project to {id,total} 200k", () => {
    const out = new Array(rows.length);
    for (let i = 0; i < rows.length; i++) {
        const r = rows[i];
        out[i] = { id: r.id, total: r.amount, city: r.city };
    }
    return out;
});

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

const t5 = bench("stringify 20k records", () => {
    const small = [];
    for (let i = 0; i < 20000; i++)
        small.push({ id: ids[i], name: names[i], city: cities[i], amount: amounts[i] });
    return JSON.stringify(small).length;
});

console.log("\n  total " + (t1 + t2 + t3 + t4 + t5).toFixed(2) + " ms");
