/* bench_json.js -- RFC 6901/6902 module costs.
 *
 * Emits `#DATA<TAB>case<TAB>ns_per_op`, loop floor subtracted, best-of-9.
 * Patch.apply pays a full deep copy per call (that IS the atomicity); the
 * small/large split shows where the money goes.
 */
import { Pointer, Patch } from "dyna:json";

const TRIALS = 9;

function bestOf(N, fn) {
    let b = Infinity;
    for (let t = 0; t < TRIALS; t++) {
        const t0 = performance.now();
        for (let i = 0; i < N; i++) fn();
        const dt = performance.now() - t0;
        if (dt < b) b = dt;
    }
    return b * 1e6 / N;
}

const FLOOR = (() => {
    let b = Infinity;
    for (let t = 0; t < TRIALS; t++) {
        const t0 = performance.now();
        let s = 0;
        for (let i = 0; i < 300000; i++) s += i;
        const dt = performance.now() - t0;
        if (dt < b) b = dt;
        if (s === -1) print("no");
    }
    return b * 1e6 / 300000;
})();

function bench(name, N, fn) {
    const per = bestOf(N, fn) - FLOOR;
    print(`${name.padEnd(44)} ${per.toFixed(2).padStart(9)} ns/op`);
    print(`#DATA\t${name}\t${per.toFixed(3)}`);
}

print(`loop floor: ${FLOOR.toFixed(2)} ns/iteration (subtracted from every row)`);
print("");

/* Pointer.get, deep: 50 nested /k tokens. */
{
    let deep = {}, dp = deep, path = "";
    for (let i = 0; i < 50; i++) { dp.k = {}; dp = dp.k; path += "/k"; }
    bench("Pointer.get deep (50 tokens)", 200000, () => Pointer.get(deep, path));
}
/* Pointer.get, wide: last of 2000 keys. */
{
    const wide = {};
    for (let i = 0; i < 2000; i++) wide["k" + i] = i;
    bench("Pointer.get wide (2000 keys)", 100000, () => Pointer.get(wide, "/k1999"));
}
/* escape / unescape on short tokens. */
{
    const T = "a/b~c";
    bench("Pointer.escape short token", 300000, () => Pointer.escape(T));
    const U = "a~1b~0c";
    bench("Pointer.unescape short token", 300000, () => Pointer.unescape(U));
}
/* Patch.apply, small: 3 ops on a tiny doc (clone is the whole cost). */
{
    const doc = {a: 1, b: [1, 2, 3], c: {x: 1}};
    const ops = [
        {op: "add", path: "/d", value: 4},
        {op: "replace", path: "/a", value: 10},
        {op: "remove", path: "/b/1"},
    ];
    bench("Patch.apply small (3 ops)", 20000, () => Patch.apply(doc, ops));
}
/* Patch.apply, large: 20 ops on 200 keys. */
{
    const doc = {};
    for (let i = 0; i < 200; i++) doc["k" + i] = {i, arr: [i, i + 1]};
    const ops = [];
    for (let i = 0; i < 20; i++) ops.push({op: "replace", path: "/k" + i + "/arr/0", value: i * 10});
    bench("Patch.apply large (20 ops / 200 keys)", 3000, () => Patch.apply(doc, ops));
}
