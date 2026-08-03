/* bench_array_ext.js — native Array methods vs hand-written JS.
 * The RATIO (native / JS) is the metric to trust under qemu emulation; the goal
 * is native < 1.0x (faster) — proving the methods beat interpreter dispatch.
 * Run: dynajs tests/bench_array_ext.js   (also under docker amd64) */

const N = 1_000_000;
const nums = new Array(N);
for (let i = 0; i < N; i++) nums[i] = (i * 2654435761) % 100000;

function best(fn, iters = 5) {
    let b = Infinity;
    for (let k = 0; k < iters; k++) {
        const t0 = performance.now();
        fn();
        const dt = performance.now() - t0;
        if (dt < b) b = dt;
    }
    return b;
}

function row(name, native, js) {
    const tn = best(native), tj = best(js);
    const ratio = tn / tj;
    print(
        name.padEnd(14) +
        " native=" + tn.toFixed(2).padStart(8) + "ms" +
        " js=" + tj.toFixed(2).padStart(8) + "ms" +
        " ratio=" + ratio.toFixed(3) + (ratio < 1 ? "  (native faster)" : "  (JS faster)")
    );
}

print("=== Array `_` methods vs JS, N=" + N + " ===");

row("sum",
    () => nums.sum(),
    () => { let s = 0; for (let i = 0; i < nums.length; i++) s += nums[i]; return s; });

row("min",
    () => nums.min(),
    () => { let m = Infinity; for (let i = 0; i < nums.length; i++) if (nums[i] < m) m = nums[i]; return m; });

row("count(pred)",
    () => nums.count(x => x < 50000),
    () => nums.filter(x => x < 50000).length);

row("any(pred)",
    () => nums.any(x => x === 99999),
    () => nums.some(x => x === 99999));

row("take",
    () => nums.take(1000),
    () => nums.slice(0, 1000));

row("compact",
    () => nums.compact(),
    () => nums.filter(x => x !== null && x !== undefined));

/* batch 3: sortBy (numeric key) vs Array.sort with a comparator */
row("sortBy",
    () => nums.sortBy(),
    () => nums.slice().sort((a, b) => a - b));

/* groupBy (mod-10 buckets) vs a hand-written reduce */
row("groupBy",
    () => nums.groupBy(x => x % 10),
    () => nums.reduce((acc, x) => { const k = x % 10; (acc[k] || (acc[k] = [])).push(x); return acc; }, {}));

/* batch 4: dedup + set-ops. Compare vs the idiomatic Set-based JS (both O(n)). */
const dup = new Array(N);
for (let i = 0; i < N; i++) dup[i] = i % 50000;   /* each value ~20x */
const other = []; for (let i = 0; i < 50000; i++) other.push(i * 2);

row("unique",
    () => dup.unique(),
    () => [...new Set(dup)]);
row("intersect",
    () => dup.intersect(other),
    () => { const s = new Set(other); return [...new Set(dup)].filter(x => s.has(x)); });
row("union",
    () => dup.union(other),
    () => [...new Set([...dup, ...other])]);

/* ---- batch 7: structural transforms (fast-array bulk-copy path) vs JS ---- */
row("splitEvery",
    () => nums.splitEvery(1000),
    () => { const r = []; for (let i = 0; i < nums.length; i += 1000) r.push(nums.slice(i, i + 1000)); return r; });
row("aperture",
    () => nums.aperture(3),
    () => { const r = []; for (let i = 0; i + 3 <= nums.length; i++) r.push(nums.slice(i, i + 3)); return r; });
row("splitAt",
    () => nums.splitAt(N / 2),
    () => [nums.slice(0, N / 2), nums.slice(N / 2)]);
row("update",
    () => nums.update(N - 1, 42),
    () => { const c = nums.slice(); c[N - 1] = 42; return c; });
row("move",
    () => nums.move(0, N - 1),
    () => { const c = nums.slice(); const [x] = c.splice(0, 1); c.splice(N - 1, 0, x); return c; });

/* ---- TypedArray SIMD reductions vs a scalar JS loop over the buffer ---- */
print("=== TypedArray `_` SIMD reductions vs JS loop, N=" + N + " ===");
const f64 = new Float64Array(N);
for (let i = 0; i < N; i++) f64[i] = (i * 2654435761 % 100000) * 0.5;
const i32 = new Int32Array(N);
for (let i = 0; i < N; i++) i32[i] = (i * 2654435761) | 0;

row("f64 _sum(SIMD)",
    () => f64.sum(),
    () => { let s = 0; for (let i = 0; i < f64.length; i++) s += f64[i]; return s; });
row("f64 _min(SIMD)",
    () => f64.min(),
    () => { let m = Infinity; for (let i = 0; i < f64.length; i++) if (f64[i] < m) m = f64[i]; return m; });
row("f64 _max(SIMD)",
    () => f64.max(),
    () => { let m = -Infinity; for (let i = 0; i < f64.length; i++) if (f64[i] > m) m = f64[i]; return m; });
row("i32 _sum(SIMD)",
    () => i32.sum(),
    () => { let s = 0; for (let i = 0; i < i32.length; i++) s += i32[i]; return s; });

print("done");
