import { DataFrame } from "dyna:dataframe";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }

const f64 = (...a) => new Float64Array(a);
const u8 = (...a) => new Uint8Array(a);

const df = new DataFrame({
    a: f64(1, 2, 3, 4, 5, 6, 7, 8),
    t: f64(0, 1, 2, 3, 10, 11, 12, 13),
    tu: f64(0, 5, 1, 2, 3, 4, 6, 7),
    s: ["x", "y", "x", "z", "x", "y", "z", "x"],
});
const empty = new DataFrame({ a: f64(), t: f64(), s: [] });
const mask = u8(1, 0, 1, 0, 1, 0, 1, 0);

{
    const r = df.RESAMPLE("t", 4, "sum");
    const cols = r.TO_COLUMNS();
    assert(cols.bucket.length === 3, "three buckets at interval 4");
    assert(cols.bucket[0] === 0 && cols.bucket[1] === 8 && cols.bucket[2] === 12,
        "bucket starts are floor(t/4)*4");
    assert(cols.value[0] === 6 && cols.value[1] === 21 && cols.value[2] === 25,
        "bucket sums of the time column");
    const e = empty.RESAMPLE("t", 1, "sum").TO_COLUMNS();
    assert(e.bucket.length === 0 && e.value.length === 0,
        "the empty frame returns the two-column zero-row shape");
}

let refuses = 0, ops = 0;
for (let i = 0; i < 2500; i++) {
    try {
        df.RESAMPLE("tu", 2, "sum");
        throw new Error("unsorted RESAMPLE must refuse");
    } catch (e) {
        if (!(e instanceof RangeError)) throw e;
        refuses++;
    }
    df.RESAMPLE("t", 4, "sum");
    df.RESAMPLE("t", 4, "mean");
    df.RESAMPLE("t", 4, "min");
    df.RESAMPLE("t", 4, "max");
    df.RESAMPLE("t", 4, "count");
    empty.RESAMPLE("t", 1, "sum");
    empty.RESAMPLE("t", 1, "mean");
    try { df.RESAMPLE("t", 0, "sum"); } catch (e) {  }
    try { df.RESAMPLE("t", 2, "bogus"); } catch (e) {  }
    try { df.RESAMPLE("s", 2, "sum"); } catch (e) {  }
    ops += 8;
    switch (i % 10) {
    case 0: df.ABS("a"); df.CLIP("a", 2, 6); df.FILL_NA("a", 0); break;
    case 1: df.ADD("a", "a"); df.MUL("a", 2); df.POW("a", 2); break;
    case 2: df.SORT("a"); df.ARG_SORT("a"); df.RANK("a"); break;
    case 3: df.QUANTILE("a", 0.5); df.N_LARGEST("a", 3); df.UNIQUE("a"); break;
    case 4: df.GROUP_BY_SUM("s", "a"); df.VALUE_COUNTS("s"); df.DROP_DUPLICATES("s"); break;
    case 5: df.CUM_SUM("a"); df.SHIFT("a", 1); df.DIFF("a", 1); break;
    case 6: df.ROLLING_SUM("a", 3); df.EMA("a", 0.5); df.ZSCORE("a"); break;
    case 7: df.FILTER(mask); df.SLICE(1, 5); df.MASK(mask); break;
    case 8: df.PIVOT("s", "t", "a", "sum"); df.MELT(["s"], ["a", "t"]); break;
    case 9: df.HISTOGRAM("a", 4); df.QUANTILES("a", [0, 0.5, 1]); df.DESCRIBE("a"); break;
    }
    ops += 1;
}
assert(refuses === 2500, "every unsorted RESAMPLE refused: " + refuses);
assert(ops >= 10000, "the churn ran " + ops + " operations");

print("test_dataframe_churn: all " + n + " assertions passed (" + ops + " ops)");
