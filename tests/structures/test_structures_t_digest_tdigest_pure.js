// covers: dyna:structures.TDigest
// TDigest reads are pure. quantile/quantiles/cdf/centroids, merge's source and serialize() used to flush the insert
// buffer as a side effect, and a flushed digest compresses later values differently -- so a digest that was
// queried or checkpointed mid-stream gave different later estimates than one that was not (p95 184.04 vs 184.64
// on 390 values). Every check compares a touched digest against an untouched twin fed the same values: estimates
// must be identical to the bit, at every cut point.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_t_digest_tdigest_pure");
import { TDigest } from "dyna:structures";
import { CRC32C } from "dyna:crypto";

let failures = 0,
    passed = 0;
function ok(cond, msg) {
    if (cond) passed++;
    else {
        failures++;
        if (failures < 30) print("FAIL " + msg);
    }
}
let seed = 0x2545f491;
const rnd = () => ((seed = (Math.imul(seed ^ (seed >>> 15), 0x2c1b3c6d) + 0x6d2b79f5) >>> 0) / 2 ** 32);
const QS = [0, 0.001, 0.01, 0.05, 0.1, 0.25, 0.5, 0.75, 0.9, 0.95, 0.99, 0.999, 1];
const sameBits = (a, b) => Object.is(a, b);
function agree(a, b) {
    for (const q of QS) if (!sameBits(a.quantile(q), b.quantile(q))) return `quantile(${q}) ${a.quantile(q)} vs ${b.quantile(q)}`;
    for (const x of [-1, 10, 100, 184, 250, 1e6]) if (!sameBits(a.cdf(x), b.cdf(x))) return `cdf(${x})`;
    if (a.centroids !== b.centroids) return `centroids ${a.centroids} vs ${b.centroids}`;
    if (a.count !== b.count || !sameBits(a.mean, b.mean) || a.min !== b.min || a.max !== b.max) return "count/mean/min/max";
    return "";
}
const bytesEq = (x, y) => x.length === y.length && x.every((v, i) => v === y[i]);

for (const delta of [10, 100]) {
    // lengths straddle the buffer capacity (6 * delta + 64) so cuts land before, at and after a forced flush
    const cap = delta * 6 + 64;
    for (const len of [1, 7, 390, cap - 1, cap, cap + 1, 3 * cap + 17]) {
        const vals = Array.from({ length: len }, () => Math.exp(rnd() * 6));
        const cuts = [0, 1, Math.floor(len / 3), len - 1].filter((c, i, a) => c < len && a.indexOf(c) === i);
        for (const cut of cuts) {
            const plain = new TDigest(delta),
                queried = new TDigest(delta),
                saved = new TDigest(delta);
            for (let i = 0; i < len; i++) {
                plain.add(vals[i]);
                queried.add(vals[i]);
                saved.add(vals[i]);
                if (i === cut) {
                    queried.quantile(0.95);
                    queried.quantiles([0.1, 0.9]);
                    queried.cdf(100);
                    void queried.centroids;
                    queried.serialize();
                    new TDigest(delta).merge(queried);
                }
            }
            const why = agree(plain, queried);
            ok(!why, `delta ${delta} len ${len}: queried at ${cut} estimates like the untouched digest (${why})`);
            // checkpoint at the cut, restore, continue: identical to never stopping
            const half = new TDigest(delta);
            for (let i = 0; i <= cut; i++) half.add(vals[i]);
            const restored = TDigest.deserialize(half.serialize());
            ok(!agree(restored, half), `delta ${delta} len ${len}: restore at ${cut} equals the original (${agree(restored, half)})`);
            for (let i = cut + 1; i < len; i++) restored.add(vals[i]);
            const w2 = agree(plain, restored);
            ok(!w2, `delta ${delta} len ${len}: restored at ${cut} and continued estimates like the untouched digest (${w2})`);
            ok(bytesEq(plain.serialize(), restored.serialize()), `delta ${delta} len ${len} cut ${cut}: records are byte-identical`);
        }
    }
}

__kitS.section("merge leaves the source's record unchanged", () => {
    // merge(other) leaves other's state alone, and the target ends as if it had merged an untouched copy
    const vals = Array.from({ length: 500 }, () => rnd() * 1000);
    const a = new TDigest(50),
        b = new TDigest(50);
    for (const v of vals) {
        a.add(v);
        b.add(v);
    }
    const before = a.serialize();
    const t1 = new TDigest(50).merge(a);
    ok(bytesEq(before, a.serialize()), "merge leaves the source's record unchanged");
    ok(!agree(a, b), `merge leaves the source estimating like its twin (${agree(a, b)})`);
    const t2 = new TDigest(50).merge(b);
    ok(!agree(t1, t2), "two merges of equal sources agree");
});

__kitS.section("an earlier-format record loads:", () => {
    // a record without the buffer (the earlier format: delta positive, no buffer count) still loads
    const d = new TDigest(20);
    const rec = d.serialize();
    // record = 20-byte header (the last 8 bytes are the body length, u64 LE), body, CRC32C of everything before it (u32 LE);
    // the body of an empty digest is -delta, min, max, sum (4 x f64 LE), n and nbuf (2 x u32 LE)
    const head = rec.length - 4 - 40;
    const dv = new DataView(rec.buffer, rec.byteOffset + head, 8);
    ok(dv.getFloat64(0, true) === -20, `the record's body starts with -delta (got ${dv.getFloat64(0, true)})`);
    const cents = [
        [1, 2],
        [5, 1],
        [9, 3],
    ];
    const old = new Uint8Array(head + 32 + 4 + 16 * cents.length + 4);
    old.set(rec.subarray(0, head));
    const ov = new DataView(old.buffer);
    [20, 1, 9, 1 * 2 + 5 + 27].forEach((v, i) => ov.setFloat64(head + 8 * i, v, true));
    ov.setUint32(head + 32, cents.length, true);
    cents.forEach(([m, w], i) => {
        ov.setFloat64(head + 36 + 16 * i, m, true);
        ov.setFloat64(head + 44 + 16 * i, w, true);
    });
    ov.setBigUint64(head - 8, BigInt(old.length - 4 - head), true);
    ov.setUint32(old.length - 4, CRC32C(old.subarray(0, old.length - 4)) >>> 0, true);
    let t = null;
    try {
        t = TDigest.deserialize(old);
    } catch (e) {
        ok(false, "an earlier-format record loads: " + e);
    }
    if (t) {
        ok(t.count === 6 && t.min === 1 && t.max === 9 && t.mean === 34 / 6, `earlier-format record: count/min/max/mean`);
        ok(t.centroids === 3 && t.quantile(0) === 1 && t.quantile(1) === 9, "earlier-format record: centroids and ends");
    }
    // a buffered record holding one value more than the capacity (6 * delta + 64) is refused; every entry's bytes
    // are present and valid, so only the capacity rule can reject it
    const forge = (count) => {
        const body = 32 + 8 + 16 * count,
            out = new Uint8Array(head + body + 4);
        out.set(rec.subarray(0, head));
        const v = new DataView(out.buffer);
        v.setBigUint64(head - 8, BigInt(body), true);
        [-10, 0, 1, count / 2].forEach((x, i) => v.setFloat64(head + 8 * i, x, true));
        v.setUint32(head + 32, 0, true);
        v.setUint32(head + 36, count, true);
        for (let i = 0; i < count; i++) {
            v.setFloat64(head + 40 + 16 * i, 0.5, true);
            v.setFloat64(head + 48 + 16 * i, 1, true);
        }
        v.setUint32(out.length - 4, CRC32C(out.subarray(0, out.length - 4)) >>> 0, true);
        return out;
    };
    let full = null;
    try {
        full = TDigest.deserialize(forge(124));
    } catch (e) {
        ok(false, "a record exactly at capacity loads: " + e);
    }
    ok(full && full.count === 124 && full.quantile(0.5) === 0.5, "a record exactly at capacity loads");
    let refused = false;
    try {
        TDigest.deserialize(forge(125));
    } catch {
        refused = true;
    }
    ok(refused, "a record whose buffer count exceeds the capacity is refused");
});
__kitS.end();
print(`tdigest pure reads: ${passed} passed, ${failures} failed`);
if (failures) throw new Error(`${failures} failures`);
