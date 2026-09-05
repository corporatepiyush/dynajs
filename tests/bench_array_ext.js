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

/* --- zip: presized fast arm vs the generic property-define arm ---
 * ZN is a tenth of N because zip allocates one pair array PER ELEMENT; at 1e6
 * the row measures the allocator and the collector, not the change. */
const ZN = N / 10;
const zipA = nums.slice(0, ZN);
const zipB = new Array(ZN);
for (let i = 0; i < ZN; i++) zipB[i] = ZN - i;
row("zip",
    () => zipA.zip(zipB),
    () => { const r = new Array(ZN); for (let i = 0; i < ZN; i++) r[i] = [zipA[i], zipB[i]]; return r; });

/* CONTROL, bypass-never-fires: a hole makes the receiver a non-fast array, so
 * this MUST take the generic arm and MUST NOT move. If it moves, the generic
 * path was not preserved byte-for-byte and the fast arm is swallowing it. */
const zipHoled = zipA.slice(); delete zipHoled[ZN >> 1];
row("zip(generic)",
    () => zipHoled.zip(zipB),
    () => { const r = new Array(ZN); for (let i = 0; i < ZN; i++) r[i] = [zipHoled[i], zipB[i]]; return r; });

/* --- median: quickselect O(n) vs the full sort it replaced ---
 * The JS baseline is a full sort, which is what the native code USED to do, so
 * the ratio is directly the algorithmic change. `nums` holds only 100k distinct
 * values across 1e6 slots, so the three-way partition's equal band is well
 * exercised rather than being a degenerate all-distinct case. */
row("median",
    () => nums.median(),
    () => { const c = nums.slice().sort((a, b) => a - b); return (c[c.length / 2 - 1] + c[c.length / 2]) / 2; });

/* The row where quickselect can LOSE: at n=64 the insertion-sort cutoff and the
 * median-of-three setup are pure overhead against libc qsort on 64 doubles.
 * Publish it either way -- reporting only the winning size is how a regression
 * ships. */
const small = nums.slice(0, 64);
row("median(n=64)",
    () => { let s = 0; for (let i = 0; i < 20000; i++) s += small.median(); return s; },
    () => { let s = 0; for (let i = 0; i < 20000; i++) { const c = small.slice().sort((a, b) => a - b); s += (c[31] + c[32]) / 2; } return s; });

print("done");
