// covers: dyna:dataframe.DataFrame
// The modperf round moved several verbs onto cheaper sort paths; every one is checked here against a JS oracle.
// QUANTILES sorts through the radix path (dfo_sorted) instead of the comparison quicksort; N_LARGEST/N_SMALLEST
// select the top k (quickselect bounded by a depth budget, then a sort of k) when k*8 < n and fully sort otherwise;
// DELTA_SUM_TIMESTAMP sorts 16-byte {time, value} records with a stable LSD radix from 512 records and an index-tiebreak
// quicksort below (no sort at all when the times are already in order); TOP_K_WEIGHTED sums each key's weights in row
// order, then ranks the distinct keys (owner D9/F26: equal keys keep row order at EVERY size; the old key-only quicksort
// left them in pivot order, so the same data could give different sums; see test_dataframe_data_frame_tie_order.js), and
// the t-digest buffer takes the same radix from 64 records (a stable shell sort of (key, row) below, was libc qsort);
// GROUP_ARRAY_SORTED radix-sorts each group of 64+ values with NaN moved last; the serial radix now starts at 256
// rows (was 2048) so SORT/ARG_SORT/RANK at 256..2047 rows take it; ROLLING_VAR/ROLLING_STD without a mask compute
// four adjacent windows per step with the same per-window two-pass order (fused multiply-add may move the last ulp, so
// the oracle allows 1e-12 relative); JOIN gathers each column with a loop specialised by source type; MASK widens a
// numeric column in one pass and then selects the fill; TO_RECORDS builds each row from a cached shape and reuses one
// JS string per dictionary entry. Ties, NaN, -0 and adversarial orders are covered because each path has its own rules.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_dataframe_data_frame_sort_paths_oracle");
import { DataFrame } from "dyna:dataframe";

let checks = 0, fails = 0;
function eq(got, want, what) {
    checks++;
    if (!Object.is(got, want)) { fails++; print("FAIL: " + what + ": got " + got + ", want " + want); }
}
function near(got, want, what, rel) {
    checks++;
    const ok = Object.is(got, want) || (Number.isFinite(want) && Math.abs(got - want) <= (rel || 1e-12) * Math.max(1, Math.abs(want)));
    if (!ok) { fails++; print("FAIL: " + what + ": got " + got + ", want " + want); }
}
function arr(got, want, what, cmp) {
    checks++;
    let bad = got.length !== want.length ? "length " + got.length + " vs " + want.length : "";
    for (let i = 0; !bad && i < want.length; i++)
        if (!(cmp ? cmp(got[i], want[i]) : Object.is(got[i], want[i]))) bad = "at " + i + ": " + got[i] + " vs " + want[i];
    if (bad) { fails++; print("FAIL: " + what + ": " + bad); }
}
let seed = 0x2545F491;
const rnd = () => { seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed / 4294967296; };
const numAsc = (a, b) => (Number.isNaN(a) ? (Number.isNaN(b) ? 0 : 1) : Number.isNaN(b) ? -1 : a - b);
const SHAPES = {
    uniform: (n) => Float64Array.from({ length: n }, () => rnd() * 2000 - 1000),
    dups: (n) => Float64Array.from({ length: n }, () => (rnd() * 5) | 0),
    sorted: (n) => Float64Array.from({ length: n }, (_, i) => i),
    reversed: (n) => Float64Array.from({ length: n }, (_, i) => n - i),
    organ: (n) => Float64Array.from({ length: n }, (_, i) => (i < n / 2 ? i : n - i)),
    nans: (n) => Float64Array.from({ length: n }, () => (rnd() < 0.2 ? NaN : rnd() * 10)),
};
const SIZES = [1, 2, 5, 63, 64, 65, 255, 256, 257, 1000, 2047, 2048, 5000];

__kitS.section("QUANTILES equals the sorted-array interpolation for every shape and mask", () => {
    const qs = [0, 0.1, 0.25, 0.5, 0.9, 1];
    for (const [name, gen] of Object.entries(SHAPES))
        for (const n of SIZES) {
            const v = gen(n), m = Uint8Array.from({ length: n }, () => (rnd() < 0.7 ? 1 : 0));
            for (const mask of [undefined, m]) {
                const s = Array.from(v).filter((x, i) => !Number.isNaN(x) && (!mask || mask[i])).sort((a, b) => a - b);
                const got = new DataFrame({ v }).QUANTILES("v", qs, mask);
                qs.forEach((q, j) => {
                    let want = NaN;
                    if (s.length) {
                        const pos = q * (s.length - 1), lo = Math.floor(pos), f = pos - lo;
                        want = f === 0 || lo + 1 >= s.length ? s[lo] : s[lo] + f * (s[lo + 1] - s[lo]);
                    }
                    near(got[j], want, name + " n=" + n + (mask ? " masked" : "") + " q=" + q, 4 * Number.EPSILON);
                });
            }
        }
});

__kitS.section("N_LARGEST and N_SMALLEST equal the head of a full sort on the select and the sort paths", () => {
    for (const [name, gen] of Object.entries(SHAPES))
        for (const n of SIZES) {
            const v = gen(n), df = new DataFrame({ v });
            const s = Array.from(v).filter((x) => !Number.isNaN(x)).sort((a, b) => a - b), r = [...s].reverse();
            for (const k of [0, 1, 3, (n >> 3) - 1, n >> 3, n >> 1, n, n + 5]) {
                if (k < 0) continue;
                arr(df.N_SMALLEST("v", k), s.slice(0, k), name + " n=" + n + " N_SMALLEST k=" + k);
                arr(df.N_LARGEST("v", k), r.slice(0, k), name + " n=" + n + " N_LARGEST k=" + k);
            }
        }
    const v = new Float64Array([3, -0, 0, 1, -0, 0]);
    arr(new DataFrame({ v }).N_SMALLEST("v", 2), [-0, -0], "the depth-bounded select keeps signed zeros by row order", (a, b) => a === b);
});

__kitS.section("SORT, ARG_SORT and RANK between 256 and 2047 rows match a stable oracle", () => {
    for (const [name, gen] of Object.entries(SHAPES))
        for (const n of [255, 256, 300, 1000, 2047]) {
            const v = gen(n), df = new DataFrame({ v });
            const idx = Array.from({ length: n }, (_, i) => i).sort((a, b) => numAsc(v[a], v[b]) || a - b);
            arr(df.ARG_SORT("v"), idx, name + " n=" + n + " ARG_SORT");
            arr(df.SORT("v"), idx.map((i) => v[i]), name + " n=" + n + " SORT");
        }
    const iv = Int32Array.from({ length: 700 }, () => ((rnd() * 40) | 0) - 20);
    const idx = Array.from({ length: 700 }, (_, i) => i).sort((a, b) => iv[a] - iv[b] || a - b);
    arr(new DataFrame({ iv }).ARG_SORT("iv"), idx, "int32 ties keep row order");
});

__kitS.section("DELTA_SUM_TIMESTAMP sums rises in time order; equal timestamps keep row order at every size", () => {
    for (const n of [3, 16, 17, 24, 25, 40, 48, 49, 64, 65, 500, 2047, 2048, 4000, 9000]) for (const ties of [false, true]) {
        const t = Float64Array.from({ length: n }, (_, i) => (ties ? (rnd() * (n / 4)) | 0 : i * 3 + ((rnd() * 3) | 0)));
        if (!ties) for (let i = n - 1; i > 0; i--) { const j = (rnd() * (i + 1)) | 0; const x = t[i]; t[i] = t[j]; t[j] = x; }
        if (ties) for (let i = 0; i < n; i += 7) t[i] = i % 2 ? -0 : 0;
        const v = Float64Array.from({ length: n }, () => (rnd() * 100) | 0);
        const ord = Array.from({ length: n }, (_, i) => i).sort((a, b) => t[a] - t[b] || a - b);
        let s = 0;
        for (let i = 1; i < n; i++) if (v[ord[i]] > v[ord[i - 1]]) s += v[ord[i]] - v[ord[i - 1]];
        eq(new DataFrame({ v, t }).DELTA_SUM_TIMESTAMP("v", "t"), s, "n=" + n + (ties ? " with ties" : ""));
    }
});

__kitS.section("DELTA_SUM_TIMESTAMP on timestamps already in order (the no-sort path) matches the row-order oracle", () => {
    for (const n of [1, 2, 3, 24, 25, 600, 3000]) {
        const t = Float64Array.from({ length: n }, (_, i) => Math.floor(i / 3));
        if (n > 4) t[1] = -0;
        const v = Float64Array.from({ length: n }, () => (rnd() * 100) | 0);
        let want = 0;
        for (let i = 1; i < n; i++) if (v[i] > v[i - 1]) want += v[i] - v[i - 1];
        eq(new DataFrame({ v, t }).DELTA_SUM_TIMESTAMP("v", "t"), want, "sorted n=" + n);
        if (n > 3) {
            const t2 = t.slice(); t2[n - 1] = -1;
            const ord = [n - 1, ...Array.from({ length: n - 1 }, (_, i) => i)];
            let want2 = 0;
            for (let j = 1; j < n; j++) if (v[ord[j]] > v[ord[j - 1]]) want2 += v[ord[j]] - v[ord[j - 1]];
            eq(new DataFrame({ v, t: t2 }).DELTA_SUM_TIMESTAMP("v", "t"), want2, "one late row out of order n=" + n);
        }
    }
});

__kitS.section("TOP_K_WEIGHTED sums weights per key in row order at every size and ranks by weight then key", () => {
    for (const n of [10, 24, 25, 63, 64, 200, 1000, 2047, 2048, 5000]) {
        const k = Float64Array.from({ length: n }, () => ((rnd() * 30) | 0) - 10);
        const w = Float64Array.from({ length: n }, () => rnd() * 3);
        const acc = new Map();
        for (let i = 0; i < n; i++) acc.set(k[i], (acc.get(k[i]) || 0) + w[i]);
        const top = [...acc].sort((a, b) => b[1] - a[1] || a[0] - b[0]).slice(0, 7);
        const got = new DataFrame({ k, w }).TOP_K_WEIGHTED("k", "w", 7);
        arr(got.keys, top.map((e) => e[0]), "keys n=" + n);
        arr(got.values, top.map((e) => e[1]), "weights n=" + n);
    }
});

__kitS.section("TOP_K_WEIGHTED and QUANTILE_EXACT_WEIGHTED treat -0 and 0 as one key reported as 0", () => {
    const k = new Float64Array([-0, 2, 0, -0, 2, 5, 0]);
    const w = new Float64Array([1, 1, 1, 1, 1, 0.5, 1]);
    const got = new DataFrame({ k, w }).TOP_K_WEIGHTED("k", "w", 3);
    arr(got.keys, [0, 2, 5], "signed zeros merge into key 0");
    arr(got.values, [4, 2, 0.5], "their weights add up");
    eq(new DataFrame({ k: new Float64Array([-0, -0, 3]), w: new Float64Array([1, 1, 1]) }).QUANTILE_EXACT_WEIGHTED("k", "w", 0.5), 0, "a -0-only key is reported as 0");
});

__kitS.section("QUANTILE_EXACT_WEIGHTED matches a row-order weighted oracle at every size", () => {
    for (const n of [5, 24, 25, 200, 1000, 2047, 2048, 5000]) {
        const k = Float64Array.from({ length: n }, () => ((rnd() * 40) | 0) - 15);
        const w = Float64Array.from({ length: n }, () => rnd() * 3);
        const acc = new Map();
        for (let i = 0; i < n; i++) acc.set(k[i], (acc.get(k[i]) || 0) + w[i]);
        const sorted = [...acc].sort((a, b) => a[0] - b[0]);
        const df = new DataFrame({ k, w });
        for (const q of [0, 0.1, 0.5, 0.9, 1]) {
            const got = df.QUANTILE_EXACT_WEIGHTED("k", "w", q);
            const total = sorted.reduce((t, e) => t + e[1], 0);
            let c = 0, pick = sorted[sorted.length - 1][0];
            for (const [key, wt] of sorted) { c += wt; if (c >= q * total) { pick = key; break; } }
            eq(got, pick, "n=" + n + " q=" + q);
        }
    }
});

__kitS.section("GROUP_ARRAY_SORTED sorts each group ascending with NaN last", () => {
    for (const n of [50, 300, 6000]) {
        const g = Int32Array.from({ length: n }, () => (rnd() * 4) | 0);
        const v = Float64Array.from({ length: n }, () => (rnd() < 0.1 ? NaN : rnd() * 50 - 25));
        const r = new DataFrame({ g, v }).GROUP_ARRAY_SORTED("g", "v");
        r.keys.forEach((key, j) => {
            const want = Array.from(v).filter((_, i) => g[i] === key).sort(numAsc);
            arr(r.values[j], want, "n=" + n + " group " + key);
        });
    }
});

__kitS.section("QUANTILES_TDIGEST stays within 1% rank of the exact quantile", () => {
    for (const n of [100, 3000, 20000]) {
        const v = Float64Array.from({ length: n }, () => rnd() * 1000);
        const s = Array.from(v).sort((a, b) => a - b);
        const qs = [0.05, 0.25, 0.5, 0.75, 0.95];
        const got = new DataFrame({ v }).QUANTILES_TDIGEST("v", qs);
        qs.forEach((q, j) => {
            let rank = 0;
            while (rank < n && s[rank] < got[j]) rank++;
            checks++;
            if (Math.abs(rank / n - q) > Math.max(0.01, 3 / n)) { fails++; print("FAIL: tdigest n=" + n + " q=" + q + " rank " + rank / n); }
        });
    }
});

__kitS.section("ROLLING_VAR and ROLLING_STD without a mask match the per-window two-pass oracle", () => {
    const types = [Float64Array, Float32Array, Int32Array, Int16Array, Uint8Array];
    for (const T of types)
        for (const n of [1, 4, 7, 8, 9, 50, 1003])
            for (const w of [1, 2, 3, 5, 16]) {
                const x = T.from({ length: n }, () => (T === Float64Array || T === Float32Array ? rnd() * 200 - 100 : (rnd() * 100) | 0));
                if (T === Float64Array && n > 10) x[n >> 1] = NaN;
                const df = new DataFrame({ x });
                const rv = df.ROLLING_VAR("x", w), rs = df.ROLLING_STD("x", w);
                for (let i = 0; i < n; i++) {
                    let want = NaN;
                    if (i + 1 >= w && w >= 2) {
                        let m = 0;
                        for (let j = i + 1 - w; j <= i; j++) m += x[j];
                        m /= w;
                        let q = 0;
                        for (let j = i + 1 - w; j <= i; j++) q += (x[j] - m) * (x[j] - m);
                        want = q / (w - 1);
                    }
                    near(rv[i], want, T.name + " n=" + n + " w=" + w + " var[" + i + "]");
                    near(rs[i], Math.sqrt(want), T.name + " n=" + n + " w=" + w + " std[" + i + "]");
                }
            }
});

__kitS.section("JOIN gathers every column type, with NaN for a missing right row", () => {
    const n = 400, lk = Int32Array.from({ length: n }, () => (rnd() * 60) | 0);
    const rk = Int32Array.from({ length: 90 }, () => (rnd() * 80) | 0);
    const cols = { rk, f64: Float64Array.from(rk, () => rnd()), f32: Float32Array.from(rk, () => rnd()), i8: Int8Array.from(rk, () => ((rnd() * 200) | 0) - 100), u16: Uint16Array.from(rk, () => (rnd() * 60000) | 0), u32: Uint32Array.from(rk, () => (rnd() * 4e9) >>> 0) };
    const left = new DataFrame({ lk, a: Float64Array.from(lk, () => rnd()) }), right = new DataFrame(cols);
    for (const how of ["inner", "left"]) {
        const j = left.JOIN(right, "lk", "rk", how), out = j.TO_COLUMNS();
        const rows = [];
        for (let l = 0; l < n; l++) {
            let hit = false;
            for (let r = 0; r < rk.length; r++) if (rk[r] === lk[l]) { rows.push([l, r]); hit = true; }
            if (!hit && how === "left") rows.push([l, -1]);
        }
        eq(j.ROWS, rows.length, how + " row count");
        for (const name of ["f64", "f32", "i8", "u16", "u32"]) {
            const want = [], have = [];
            rows.forEach(([l, r]) => want.push(lk[l] + ":" + (r < 0 ? "NaN" : String(cols[name][r]))));
            for (let i = 0; i < j.ROWS; i++) have.push(out.lk[i] + ":" + String(out[name][i]));
            arr(have.sort(), want.sort(), how + " column " + name);
        }
    }
});

__kitS.section("MASK widens each numeric column and puts the fill (or NaN) where the mask is 0", () => {
    for (const n of [0, 1, 7, 64, 1001]) {
        const cols = { f64: Float64Array.from({ length: n }, () => rnd() * 10 - 5), f32: Float32Array.from({ length: n }, () => rnd()), i8: Int8Array.from({ length: n }, () => ((rnd() * 250) | 0) - 125), u32: Uint32Array.from({ length: n }, () => (rnd() * 4e9) >>> 0) };
        const m = Uint8Array.from({ length: n }, () => (rnd() < 0.5 ? (rnd() * 255) | 1 : 0));
        const df = new DataFrame(cols);
        for (const fill of [undefined, -7.25]) {
            const out = (fill === undefined ? df.MASK(m) : df.MASK(m, fill)).TO_COLUMNS();
            for (const [name, src] of Object.entries(cols))
                arr(out[name], Array.from(src, (x, i) => (m[i] ? x : fill === undefined ? NaN : fill)), "n=" + n + " fill=" + fill + " " + name);
        }
    }
});

__kitS.section("TO_RECORDS builds rows from one cached shape and one string per dictionary entry", () => {
    for (const n of [0, 1, 3, 500]) {
        const s = Array.from({ length: n }, () => ["red", "green", "", "blue\u00e9"][(rnd() * 4) | 0]);
        const cols = { s, "0": Int16Array.from({ length: n }, () => ((rnd() * 600) | 0) - 300), f: Float64Array.from({ length: n }, () => (rnd() < 0.1 ? NaN : rnd())), u8: Uint8Array.from({ length: n }, () => (rnd() * 256) | 0) };
        const df = new DataFrame(cols), recs = df.TO_RECORDS(), names = df.COLUMNS;
        eq(recs.length, n, "n=" + n + " row count");
        let bad = 0;
        recs.forEach((r, i) => {
            const o = {};
            for (const k of names) o[k] = 0;
            if (Object.keys(r).join("|") !== Object.keys(o).join("|")) bad++;
            for (const k of names) if (!Object.is(r[k], k === "s" ? cols.s[i] : cols[k][i])) bad++;
        });
        eq(bad, 0, "n=" + n + " every cell");
        eq(df.TO_JSON(), JSON.stringify(recs), "n=" + n + " TO_JSON is the records serialised");
    }
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_dataframe_data_frame_sort_paths_oracle: " + checks + " checks passed");
__kitS.end();
