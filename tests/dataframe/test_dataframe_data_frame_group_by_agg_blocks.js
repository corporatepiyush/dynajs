// covers: dyna:dataframe.DataFrame
// M3: integer-key group-by reads the key column in place (no per-row key
// copy) and, from 2 Mi rows with at most 65536 groups, accumulates each 2^20
// row block separately (on several threads when available) and combines the
// blocks in block order. The block structure depends only on the row count,
// so results are deterministic and independent of the thread count. This test
// recomputes every aggregate with that exact order in JS and requires bit
// equality, with and without a mask, for GROUP_BY_SUM / MEAN / COUNT / MIN /
// MAX and GROUP_BY_AGG; below 2 Mi rows the order is plain row order. Group 0
// is all NaN in the first block, so combining min/max must let a later
// block's number replace a NaN partial.
import { DataFrame } from "dyna:dataframe";

let n = 0, failures = 0;
function check(cond, msg) { n++; if (!cond) { failures++; print("  FAIL:", msg); } }
const BLOCK = 1 << 20;

function ref(k, v, mask, G) {
    const N = k.length, blocked = N >= 2 * BLOCK && G <= 65536;
    const step = blocked ? BLOCK : N;
    const sum = new Float64Array(G), cnt = new Float64Array(G), mn = new Float64Array(G).fill(NaN), mx = new Float64Array(G).fill(NaN);
    for (let s = 0; s < N; s += step) {
        const bs = new Float64Array(G), bc = new Float64Array(G), bmn = new Float64Array(G).fill(NaN), bmx = new Float64Array(G).fill(NaN);
        for (let i = s; i < Math.min(N, s + step); i++) {
            if (mask && !mask[i]) continue;
            const g = k[i], x = v[i];
            bs[g] += x; bc[g]++;
            if (x < bmn[g] || bmn[g] !== bmn[g]) bmn[g] = x;
            if (x > bmx[g] || bmx[g] !== bmx[g]) bmx[g] = x;
        }
        for (let g = 0; g < G; g++) {
            if (s === 0) { sum[g] = bs[g]; } else { sum[g] += bs[g]; }
            cnt[g] += bc[g];
            if (bmn[g] < mn[g] || mn[g] !== mn[g]) mn[g] = bmn[g];
            if (bmx[g] > mx[g] || mx[g] !== mx[g]) mx[g] = bmx[g];
        }
    }
    const mean = sum.map((x, g) => cnt[g] ? x / cnt[g] : NaN);
    return { sum, mean, count: cnt, min: mn, max: mx };
}
function same(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (!Object.is(a[i], b[i])) return false;
    return true;
}

let seed = 99;
function rnd() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; }
for (const [N, G] of [[100000, 37], [(2 << 20) + 12345, 1000], [(5 << 20) + 7, 9]]) {
    const k = new Int32Array(N), v = new Float64Array(N), mask = new Uint8Array(N);
    for (let i = 0; i < N; i++) {
        k[i] = (rnd() * G) | 0;
        v[i] = (rnd() - 0.3) * 10 ** ((rnd() * 12) | 0) + (i % 97 === 0 ? 1e15 : 0);
        if (i % 1013 === 0) v[i] = NaN;
        mask[i] = rnd() < 0.7 ? 1 : 0;
    }
    k[N - 1] = G - 1;
    for (let i = 0; i < Math.min(N, BLOCK); i++) if (k[i] === 0) v[i] = NaN;
    const df = new DataFrame({ k, v });
    for (const m of [undefined, mask]) {
        const r = ref(k, v, m, G), label = N + " rows, " + G + " groups" + (m ? ", masked" : "");
        check(same(df.GROUP_BY_SUM("k", "v", m).values, r.sum), label + ": GROUP_BY_SUM bit-equal to the block-order reference");
        check(same(df.GROUP_BY_MEAN("k", "v", m).values, r.mean), label + ": GROUP_BY_MEAN");
        check(same(df.GROUP_BY_COUNT("k", m).values, r.count), label + ": GROUP_BY_COUNT");
        check(same(df.GROUP_BY_MIN("k", "v", m).values, r.min) && same(df.GROUP_BY_MAX("k", "v", m).values, r.max), label + ": GROUP_BY_MIN / MAX");
        const a = df.GROUP_BY_AGG("k", "v", ["sum", "mean", "count", "min", "max"], m);
        check(same(a.sum, r.sum) && same(a.mean, r.mean) && same(a.count, r.count) && same(a.min, r.min) && same(a.max, r.max), label + ": GROUP_BY_AGG one pass equals the single aggregates");
    }
}

print("test_dataframe_data_frame_group_by_agg_blocks: " + n + " checks, " + failures + " failures");
if (failures) throw new Error("test_dataframe_data_frame_group_by_agg_blocks: " + failures + " failures");
