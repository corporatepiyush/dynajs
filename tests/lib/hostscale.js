// Host-speed multiplier for the few suites that assert a wall-clock budget.
// A fixed pure-JS workload is timed (best of 3) against REFERENCE_MS, its cost
// on the machine the budgets were written on with 1.5x slack already folded
// in; a native host returns 1, CPU emulation returns what it really costs.
// A budget is `ms * hostScale()`: unchanged where it was tuned, honest elsewhere.
const REFERENCE_MS = 150;
let cached = 0;
function workload() {
    let h = 2166136261 >>> 0, s = 0.5, acc = "";
    const a = new Array(4096).fill(0);
    for (let i = 0; i < 1200000; i++) {
        h = Math.imul(h ^ (i & 0xff), 16777619) >>> 0;
        s = s * 1.0000001 + (h & 7) * 1e-9;
        a[h & 4095] = i;
        if ((i & 0x3fff) === 0) acc += String.fromCharCode(97 + (h % 26));
    }
    return h + s + a[7] + acc.length;
}
export function hostScale() {
    if (cached) return cached;
    let best = Infinity, sink = 0;
    for (let r = 0; r < 3; r++) {
        const t = performance.now();
        sink += workload();
        best = Math.min(best, performance.now() - t);
    }
    cached = Math.max(1, best / REFERENCE_MS) + (sink !== sink ? 1 : 0);
    return cached;
}
