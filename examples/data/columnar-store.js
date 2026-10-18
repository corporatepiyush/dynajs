// Columnar store — dictionary, run-length and bit-packed columns with zone maps and bitmap predicates, checked
// against a row-at-a-time scan of the same data.
//
// WHAT IT IS
//   The storage and scan layer of an analytical database (Parquet / ClickHouse / DuckDB in miniature):
//
//   encode      strings become dictionary codes (Interner), the sorted date column becomes runs, codes are
//               bit-packed to their minimum width (mathx.bits.len32) into Uint32Array words
//   zone maps   per-block min/max for every numeric column (simd i32Min/i32Max, f64Min/f64Max on subarrays):
//               a block whose range cannot match is never decoded
//   predicates  equality and prefix filters on strings become RoaringBitmap postings per dictionary code (Trie
//               for the prefix -> codes step); ranges on the sorted column become one addRange; AND/OR are native
//   aggregate   SUM(qty * price) GROUP BY region over the selection, plus COUNT(DISTINCT customer) with a
//               HyperLogLog and exact distinct counts from bitmap cardinality
//   oracle      every query is re-run over the original row objects with plain filters; results must match
//
// MODULES  dyna:structures (Interner, RoaringBitmap, Trie, HyperLogLog, SortedMap) · dyna:simd (i32Min, i32Max,
//          f64Min, f64Max, f64Sum, f64Dot) · dyna:mathx (bits) · dyna:compress (zstd) · dyna:random · dyna:cli
//
// RUN      dynajs examples/data/columnar-store.js [rows]

import { Interner, RoaringBitmap, Trie, HyperLogLog, SortedMap } from "dyna:structures";
import { i32Min, i32Max, f64Min, f64Max, f64Sum, f64Dot } from "dyna:simd";
import { bits } from "dyna:mathx";
import { zstd, unzstd } from "dyna:compress";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const ROWS = Number(scriptArgs[1] ?? 200_000);
const BLOCK = 4096;
const rng = new Random(2026);

// =============================================================================
// Data: a year of order lines, sorted by day as an ingest pipeline would write them
// =============================================================================
const REGIONS = ["emea-north", "emea-south", "amer-east", "amer-west", "apac-jp", "apac-au", "apac-in", "latam"];
const products = Array.from(
    { length: 600 },
    (_, i) => `${["pro", "home", "kids", "garden"][i % 4]}-${String(i).padStart(4, "0")}`,
);
const rows = [];
for (let i = 0; i < ROWS; i++) {
    const day = Math.floor((i / ROWS) * 365);
    rows.push({
        day,
        region: REGIONS[Math.min(REGIONS.length - 1, Math.floor(rng.exponential(0.6)))],
        product: products[Math.floor(products.length * rng.nextFloat() ** 2)],
        customer: rng.nextBounded(40_000),
        qty: 1 + rng.poisson(2),
        price: Math.round((5 + rng.exponential(1 / 40)) * 100) / 100,
    });
}

// =============================================================================
// Encoding
// =============================================================================
class StringColumn {
    constructor(values) {
        this.dict = new Interner();
        const codes = this.dict.internMany(values);
        this.cardinality = i32Max(new Int32Array(codes.buffer, codes.byteOffset, codes.length)) + 1;
        this.width = Math.max(1, bits.len32(this.cardinality - 1));
        this.packed = pack(codes, this.width);
        this.length = values.length;
        this.postings = Array.from({ length: this.cardinality }, () => new RoaringBitmap());
        const buckets = Array.from({ length: this.cardinality }, () => []);
        for (let i = 0; i < codes.length; i++) buckets[codes[i]].push(i);
        buckets.forEach((b, c) => this.postings[c].addMany(Uint32Array.from(b)));
        this.trie = new Trie();
        for (let c = 0; c < this.cardinality; c++) this.trie.insert(this.dict.get(c));
    }
    eq(value) {
        const c = this.dict.lookup(value);
        return c === undefined ? new RoaringBitmap() : this.postings[c].clone();
    }
    prefix(p) {
        const out = new RoaringBitmap();
        for (const v of this.trie.keysWithPrefix(p)) out.or(this.postings[this.dict.lookup(v)]);
        return out;
    }
    codes() {
        return unpack(this.packed, this.width, this.length);
    }
    codeAt(i) {
        const bit = i * this.width,
            w = bit >>> 5,
            off = bit & 31;
        let v = this.packed[w] >>> off;
        if (off + this.width > 32) v |= this.packed[w + 1] << (32 - off);
        return v & ((1 << this.width) - 1);
    }
}
function pack(values, width) {
    const words = new Uint32Array(Math.ceil((values.length * width) / 32) + 1);
    for (let i = 0, bit = 0; i < values.length; i++, bit += width) {
        const w = bit >>> 5,
            off = bit & 31;
        words[w] |= values[i] << off;
        if (off + width > 32) words[w + 1] |= values[i] >>> (32 - off);
    }
    return words;
}
function unpack(words, width, n) {
    const out = new Uint32Array(n),
        mask = width === 32 ? 0xffffffff : (1 << width) - 1;
    for (let i = 0, bit = 0; i < n; i++, bit += width) {
        const w = bit >>> 5,
            off = bit & 31;
        let v = words[w] >>> off;
        if (off + width > 32) v |= words[w + 1] << (32 - off);
        out[i] = v & mask;
    }
    return out;
}
class RunColumn {
    constructor(values) {
        const starts = [],
            vals = [];
        for (let i = 0; i < values.length; i++)
            if (i === 0 || values[i] !== values[i - 1]) {
                starts.push(i);
                vals.push(values[i]);
            }
        this.starts = Int32Array.from(starts);
        this.values = Int32Array.from(vals);
        this.length = values.length;
        this.index = new SortedMap();
        vals.forEach((v, k) => this.index.set(v, k));
    }
    // Row range [lo, hi) whose value lies in [a, b]: two ordered lookups, no scan.
    rowsBetween(a, b) {
        const first = this.index.ceilKey(a),
            last = this.index.floorKey(b);
        if (first === undefined || last === undefined || first > b) return [0, 0];
        const k1 = this.index.get(first),
            k2 = this.index.get(last);
        return [this.starts[k1], k2 + 1 < this.starts.length ? this.starts[k2 + 1] : this.length];
    }
}

const t0 = performance.now();
const store = {
    day: new RunColumn(rows.map((r) => r.day)),
    region: new StringColumn(rows.map((r) => r.region)),
    product: new StringColumn(rows.map((r) => r.product)),
    customer: Int32Array.from(rows, (r) => r.customer),
    qty: Int32Array.from(rows, (r) => r.qty),
    price: Float64Array.from(rows, (r) => r.price),
};
const zones = [];
for (let lo = 0; lo < ROWS; lo += BLOCK) {
    const hi = Math.min(ROWS, lo + BLOCK);
    zones.push({
        lo,
        hi,
        qty: [i32Min(store.qty.subarray(lo, hi)), i32Max(store.qty.subarray(lo, hi))],
        price: [f64Min(store.price.subarray(lo, hi)), f64Max(store.price.subarray(lo, hi))],
    });
}
const encodeMs = performance.now() - t0;
results["bit-packed codes round-trip"] = (() => {
    const back = store.product.codes();
    for (let i = 0; i < ROWS; i += 97) if (store.product.dict.get(back[i]) !== rows[i].product) return false;
    return store.region.dict.get(store.region.codes()[ROWS - 1]) === rows[ROWS - 1].region;
})();
results["region codes pack into 3 bits, products into 10"] = store.region.width === 3 && store.product.width === 10;
results["date column collapses to one run per day"] = store.day.values.length === 365;

// =============================================================================
// Query engine
// =============================================================================
function scan(q) {
    let sel = new RoaringBitmap();
    const [lo, hi] = store.day.rowsBetween(q.dayFrom, q.dayTo);
    sel.addRange(lo, hi);
    if (q.region) sel.and(store.region.eq(q.region));
    if (q.productPrefix) sel.and(store.product.prefix(q.productPrefix));
    let pruned = 0;
    if (q.minPrice !== undefined || q.minQty !== undefined) {
        const keep = new RoaringBitmap();
        for (const z of zones) {
            if (z.hi <= lo || z.lo >= hi) continue;
            if (
                (q.minPrice !== undefined && z.price[1] < q.minPrice) ||
                (q.minQty !== undefined && z.qty[1] < q.minQty)
            ) {
                pruned++;
                continue;
            }
            keep.addRange(z.lo, z.hi);
        }
        sel.and(keep);
    }
    const ids = sel.toArray();
    const revenue = new Float64Array(store.region.cardinality),
        count = new Int32Array(store.region.cardinality);
    const hll = new HyperLogLog(14),
        distinct = new RoaringBitmap();
    const qtyF = new Float64Array(ids.length),
        priceF = new Float64Array(ids.length);
    let n = 0;
    for (const i of ids) {
        if (q.minPrice !== undefined && store.price[i] < q.minPrice) continue;
        if (q.minQty !== undefined && store.qty[i] < q.minQty) continue;
        const c = store.region.codeAt(i);
        revenue[c] += store.qty[i] * store.price[i];
        count[c]++;
        qtyF[n] = store.qty[i];
        priceF[n] = store.price[i];
        n++;
        hll.add(String(store.customer[i]));
        distinct.add(store.customer[i]);
    }
    const groups = {};
    for (let c = 0; c < revenue.length; c++)
        if (count[c]) groups[store.region.dict.get(c)] = { revenue: revenue[c], rows: count[c] };
    return {
        groups,
        pruned,
        rows: n,
        total: f64Dot(qtyF.subarray(0, n), priceF.subarray(0, n)),
        avgPrice: n ? f64Sum(priceF.subarray(0, n)) / n : 0,
        distinct: distinct.toArray().length,
        approxDistinct: hll.count(),
    };
}
function rowScan(q) {
    const groups = {},
        customers = new Set();
    let total = 0,
        rowsHit = 0;
    for (const r of rows) {
        if (r.day < q.dayFrom || r.day > q.dayTo) continue;
        if (q.region && r.region !== q.region) continue;
        if (q.productPrefix && !r.product.startsWith(q.productPrefix)) continue;
        if (q.minPrice !== undefined && r.price < q.minPrice) continue;
        if (q.minQty !== undefined && r.qty < q.minQty) continue;
        const g = (groups[r.region] ??= { revenue: 0, rows: 0 });
        g.revenue += r.qty * r.price;
        g.rows++;
        total += r.qty * r.price;
        rowsHit++;
        customers.add(r.customer);
    }
    return { groups, total, rows: rowsHit, distinct: customers.size };
}
const same = (a, b) => {
    const ka = Object.keys(a.groups).sort(),
        kb = Object.keys(b.groups).sort();
    if (ka.join() !== kb.join() || a.rows !== b.rows || a.distinct !== b.distinct) return false;
    for (const k of ka)
        if (
            a.groups[k].rows !== b.groups[k].rows ||
            Math.abs(a.groups[k].revenue - b.groups[k].revenue) > 1e-6 * b.groups[k].revenue
        )
            return false;
    return Math.abs(a.total - b.total) <= 1e-9 * Math.max(1, b.total);
};

const queries = [
    { name: "Q1 one quarter, all regions", dayFrom: 90, dayTo: 180 },
    { name: "Q2 pro-* in amer-east, H2", dayFrom: 182, dayTo: 364, region: "amer-east", productPrefix: "pro-" },
    { name: "Q3 premium lines (price >= 400)", dayFrom: 0, dayTo: 364, minPrice: 400 },
    { name: "Q4 bulk orders of garden-00*", dayFrom: 0, dayTo: 364, productPrefix: "garden-00", minQty: 6 },
    { name: "Q5 empty: unknown region", dayFrom: 0, dayTo: 364, region: "antarctica" },
    { name: "Q6 one day", dayFrom: 200, dayTo: 200 },
];
const table = [["query", "rows", "revenue", "distinct", "hll err", "col ms", "row ms"]];
let colTotal = 0,
    rowTotal = 0,
    allSame = true,
    hllOk = true;
for (const q of queries) {
    const a0 = performance.now(),
        col = scan(q),
        a1 = performance.now(),
        row = rowScan(q),
        a2 = performance.now();
    colTotal += a1 - a0;
    rowTotal += a2 - a1;
    if (!same(col, row)) {
        allSame = false;
        print(`mismatch in ${q.name}`);
    }
    const err = col.distinct ? Math.abs(col.approxDistinct - col.distinct) / col.distinct : 0;
    if (err > 0.03) hllOk = false;
    table.push([
        q.name,
        col.rows.format(),
        col.total.format(2),
        col.distinct.format(),
        (err * 100).toFixed(2) + "%",
        (a1 - a0).toFixed(1),
        (a2 - a1).toFixed(1),
    ]);
}
print(Table(table.slice(1), { head: table[0] }));
results["columnar results equal the row scan for every query"] = allSame;
results["HyperLogLog distinct counts within 3%"] = hllOk;
results["columnar scans beat the row scan in total"] = colTotal < rowTotal;
results["empty predicate yields an empty result"] = scan(queries[4]).rows === 0;
results["zone maps prune blocks for the price predicate"] =
    scan(queries[2]).pruned > 0 || zones.every((z) => z.price[1] >= 400);

// =============================================================================
// Footprint
// =============================================================================
const enc32 = (a) => new Uint8Array(a.buffer, a.byteOffset, a.byteLength);
const columnBytes =
    store.region.packed.byteLength +
    store.product.packed.byteLength +
    store.day.starts.byteLength * 2 +
    store.customer.byteLength +
    store.qty.byteLength +
    store.price.byteLength;
const jsonBytes = new TextEncoder().encode(JSON.stringify(rows)).length;
const zProduct = zstd(enc32(store.product.packed));
results["zstd of packed codes round-trips"] = (() => {
    const back = new Uint32Array(unzstd(zProduct).buffer.slice(0));
    return back.length === store.product.packed.length && back.every((v, i) => v === store.product.packed[i]);
})();
results["columns are at least 4x smaller than JSON rows"] = columnBytes * 4 < jsonBytes;
print(
    `encode ${encodeMs.toFixed(0)} ms; columns ${columnBytes.bytes()} vs JSON ${jsonBytes.bytes()}; product codes zstd ${zProduct.length.bytes()}`,
);
print(`scan total: columnar ${colTotal.toFixed(1)} ms, row-at-a-time ${rowTotal.toFixed(1)} ms`);

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
