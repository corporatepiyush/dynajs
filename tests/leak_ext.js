function boom() { throw new Error("boom"); }
const evil = { valueOf() { return 3; }, toString() { return "k"; } };

const ITERS = (typeof scriptArgs !== "undefined" && scriptArgs[1]) ? +scriptArgs[1] : 200;
for (let it = 0; it < ITERS; it++) {
    const a = [3, 1, 4, 1, 5, 9, 2, 6, "x" + it, { id: it % 3 }, null, undefined];
    const nums = [3, 1, 4, 1, 5, 9, 2, 6];
    const objs = [{ id: it % 3 }, { id: (it + 1) % 3 }, { id: 2 }];

    a.first(); a.first(3); a.last(); a.last(3); a.isEmpty(); a.compact();
    nums.sum(); nums.average(); nums.mean(); nums.min(); nums.max();
    nums.min(x => x * 2); nums.max(evil.valueOf ? (x => -x) : 0);
    a.count(); a.count(1); a.count(x => typeof x === "number");
    a.none(1); a.any(1); a.all(x => x !== 7);
    nums.take(3); nums.drop(3); nums.takeLast(2); nums.dropLast(2);
    nums.take(999); nums.take(-1);

    nums.sortBy(); nums.sortBy(x => -x); nums.sortBy(undefined, true);
    objs.sortBy("id"); a.groupBy(x => typeof x); objs.groupBy("id");
    nums.shuffle(); nums.sample(); nums.sample(3); nums.sample(999);
    ["banana", "apple", "cherry"].sortBy();

    a.unique(); a.uniq(); [{ id: 1 }, { id: 1 }, { id: 2 }].uniqBy(x => x.id);
    objs.unique("id"); nums.intersect([2, 4, 6]); nums.intersection([1, 9]);
    nums.difference([2, 4]); nums.without([1]); nums.union([9, 10, 11]);
    ["a", "b", "a"].unique();

    const f64 = new Float64Array([3.5, -1.5, 4.5, it]);
    f64.sum(); f64.min(); f64.max(); f64.mean(); f64.average();
    new Int32Array([3, 1, 4, 1, 5]).sum();
    new Uint8Array([10, 20, 30]).max();
    new Float16Array([1.5, 2.5]).sum();

    try { nums.sortBy(boom); } catch {}
    try { nums.groupBy(boom); } catch {}
    try { [{}, {}].sortBy(() => Symbol()); } catch {}
    try { nums.unique(boom); } catch {}
    try { nums.min(boom); } catch {}
    try { new BigInt64Array([1n]).sum(); } catch {}
    try { [1, 2].intersect(boom); } catch {}
    try { a.sortBy("id"); } catch {}
    try { a.groupBy("id"); } catch {}
    try { a.unique("id"); } catch {}
    try { new Float64Array([1, 2]).dot(new Float64Array([1])); } catch {}
    try { new Float64Array([1]).dot([1]); } catch {}
    new Float64Array([1, 2, 3]).dot(new Float64Array([4, 5, 6]));
    new Int32Array([1, 2]).dot(new Int32Array([3, 4]));
}

print("leak_ext: exercised all extension methods (LSan verdict follows at exit)");
