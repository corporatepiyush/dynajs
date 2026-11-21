// covers: c:src/builtins/builtin-array.c
// Array.prototype.sort / toSorted / TypedArray.prototype.sort with a comparator: a run-adaptive stable merge sort
// (natural runs, binary insertion to minrun, galloping merges). Values are checked against a plain stable
// insertion sort written from the definition; comparator call counts pin the adaptivity (presorted input in n-1
// calls) because a quicksort gives the same answers and only the count tells them apart.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_array_sort_adaptive");
let failures = 0,
    passed = 0;
function ok(cond, msg) {
    if (cond) passed++;
    else {
        failures++;
        print("FAIL " + msg);
    }
}
let seed = 0x9e3779b9;
const rnd = () => ((seed = (Math.imul(seed ^ (seed >>> 15), 0x2c1b3c6d) + 0x6d2b79f5) >>> 0) / 2 ** 32);

function refSort(arr, cmp) {
    const a = arr.slice();
    for (let i = 1; i < a.length; i++) {
        const x = a[i];
        let j = i - 1;
        while (j >= 0 && cmp(a[j], x) > 0) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = x;
    }
    return a;
}
const byKey = (x, y) => x.k - y.k;
const same = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);

const shapes = {
    random: (n) => Array.from({ length: n }, () => Math.floor(rnd() * n)),
    sorted: (n) => Array.from({ length: n }, (_, i) => i),
    reversed: (n) => Array.from({ length: n }, (_, i) => n - i),
    "sorted+1%": (n) => Array.from({ length: n }, (_, i) => (i % 100 === 0 ? Math.floor(rnd() * n) : i)),
    "8 keys": (n) => Array.from({ length: n }, () => Math.floor(rnd() * 8)),
    sawtooth: (n) => Array.from({ length: n }, (_, i) => i % 37),
    "all equal": (n) => new Array(n).fill(5),
    organ: (n) => Array.from({ length: n }, (_, i) => (i < n / 2 ? i : n - i)),
};
// sizes straddle minrun (32..64), the insertion/merge boundary and the galloping threshold (7)
const sizes = [0, 1, 2, 3, 6, 7, 8, 31, 32, 33, 63, 64, 65, 127, 128, 129, 1000, 4097];
for (const [name, mk] of Object.entries(shapes))
    for (const n of sizes) {
        const objs = mk(n).map((k, i) => ({ k, i }));
        const want = refSort(objs, byKey);
        ok(same(objs.slice().sort(byKey), want), `${name} n=${n}: sort equals the stable reference`);
        ok(same(objs.toSorted(byKey), want), `${name} n=${n}: toSorted equals the stable reference`);
        const ta = Float64Array.from(mk(n)),
            taWant = Float64Array.from(refSort([...ta], (x, y) => y - x));
        ok(same([...ta.sort((x, y) => y - x)], [...taWant]), `${name} n=${n}: Float64Array descending comparator`);
    }

__kitS.section("sorted+1%", () => {
    // adaptivity: comparator calls at 100 000 elements
    const n = 100000,
        nlogn = n * Math.log2(n);
    const count = (arr) => {
        let c = 0;
        arr.sort((x, y) => (c++, x - y));
        return c;
    };
    const s = count(shapes.sorted(n)),
        r = count(shapes.reversed(n)),
        p = count(shapes["sorted+1%"](n)),
        q = count(shapes.random(n));
    ok(s === n - 1, `sorted input takes n-1 comparisons (got ${s})`);
    ok(r === n - 1, `strictly descending input takes n-1 comparisons (got ${r})`);
    ok(p < 0.12 * nlogn, `1% out of place takes under 0.12 n log n comparisons (got ${(p / nlogn).toFixed(3)})`);
    ok(q < 1.0 * nlogn, `random input takes under n log n comparisons (got ${(q / nlogn).toFixed(3)})`);
    let c = 0;
    Float64Array.from({ length: n }, (_, i) => i).sort((x, y) => (c++, x - y));
    ok(c === n - 1, `TypedArray: sorted input takes n-1 comparisons (got ${c})`);
});

__kitS.section("holes and undefined go last, holes deleted", () => {
    // holes and undefined: undefined after every value, holes removed to the end
    const a = [3, undefined, 1, , 2, undefined, , 0];
    a.sort((x, y) => x - y);
    ok(
        a.length === 8 && same(a.slice(0, 6), [0, 1, 2, 3, undefined, undefined]) && !(6 in a) && !(7 in a),
        "holes and undefined go last, holes deleted",
    );
    const like = { length: 5, 0: "d", 1: "a", 3: "c", 4: "b" };
    Array.prototype.sort.call(like, (x, y) => (x < y ? -1 : x > y ? 1 : 0));
    ok(like[0] === "a" && like[1] === "b" && like[2] === "c" && like[3] === "d" && !(4 in like), "array-like sorted");
});
__kitS.end();

for (const n of [5, 64, 500, 3000]) {
    // hostile comparators: whatever they answer, the result is a permutation of the input
    const base = Array.from({ length: n }, (_, i) => i);
    const isPerm = (a) => a.length === n && [...a].sort((x, y) => x - y).every((v, i) => v === i);
    const rand = base.slice().sort(() => Math.floor(rnd() * 3) - 1);
    ok(isPerm(rand), `n=${n}: a random comparator leaves a permutation`);
    ok(isPerm(base.slice().sort(() => -1)), `n=${n}: an always -1 comparator leaves a permutation`);
    ok(isPerm(base.slice().sort(() => NaN)), `n=${n}: a NaN comparator leaves a permutation`);
    // a strictly descending run costs n-1 calls, so the throw has to land inside them
    const t = base.slice();
    let calls = 0,
        threw = false;
    try {
        t.sort((x, y) => {
            if (++calls === Math.max(1, n >> 1)) throw new Error("stop");
            return y - x;
        });
    } catch (e) {
        threw = e.message === "stop";
    }
    ok(threw && same(t, base), `n=${n}: a comparator that throws propagates and leaves the array unwritten`);
    const m = base.slice();
    m.sort((x, y) => {
        m.length = 0;
        return x - y;
    });
    ok(isPerm(m), `n=${n}: a comparator that empties the array: the sorted values are written back`);
    const ta = Float64Array.from(base);
    ok(isPerm([...ta.sort(() => Math.floor(rnd() * 3) - 1)]), `n=${n}: TypedArray random comparator, permutation`);
}

print(`array sort adaptive: ${passed} passed, ${failures} failed`);
if (failures) throw new Error(`${failures} failures`);
