import { HyperLogLog } from "dyna:structures";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(a === b, m + " -- got " + a + ", want " + b); }

function within(est, truth, m, sigmas, label) {
    const err = 1.04 / Math.sqrt(m);
    const lo = truth * (1 - sigmas * err) - 1, hi = truth * (1 + sigmas * err) + 1;
    check(est >= lo && est <= hi,
          label + ": " + est.toFixed(0) + " outside [" + lo.toFixed(0) + ", " +
          hi.toFixed(0) + "] for a true " + truth);
}

{
    const h = new HyperLogLog(14);
    for (let i = 0; i < 1000; i++) h.add("a" + i);
    const before = h.count();
    within(before, 1000, 1 << 14, 3, "1k distinct");

    for (let i = 0; i < 100000; i++) h.add("b" + i);
    const after = h.count();
    check(after > before * 50,
          "add() invalidates: 1k -> 101k must move the estimate, got " +
          before.toFixed(0) + " -> " + after.toFixed(0));
    within(after, 101000, 1 << 14, 3, "101k distinct");

    const stable = h.count();
    for (let i = 0; i < 1000; i++) h.add("b" + i);
    eq(h.count(), stable, "duplicates do not move the estimate");
}

{
    const a = new HyperLogLog(14), b = new HyperLogLog(14);
    for (let i = 0; i < 5000; i++) a.add("m" + i);
    for (let i = 5000; i < 60000; i++) b.add("m" + i);

    const beforeMerge = a.count();
    within(beforeMerge, 5000, 1 << 14, 3, "before merge");

    a.merge(b);
    const afterMerge = a.count();
    check(afterMerge > beforeMerge * 5,
          "merge() invalidates: " + beforeMerge.toFixed(0) + " -> " +
          afterMerge.toFixed(0) + " (a stale cache returns the first number)");
    within(afterMerge, 60000, 1 << 14, 3, "merged union");

    const settled = a.count();
    const c = new HyperLogLog(14);
    for (let i = 0; i < 100; i++) c.add("m" + i);
    a.merge(c);
    eq(a.count(), settled, "merging a subset does not move the estimate");
}

{
    const src = new HyperLogLog(14);
    for (let i = 0; i < 50000; i++) src.add("d" + i);
    const want = src.count();

    const back = HyperLogLog.deserialize(src.serialize());
    const got = back.count();
    check(got > 0, "a decoded sketch does not report zero (got " + got + ")");
    eq(got, want, "a decoded sketch reports exactly the source's estimate");

    eq(back.count(), want, "a decoded sketch is stable across calls");
    back.add("brand new key that cannot be present");
    check(back.count() >= want, "a decoded sketch still accepts adds");
}

{
    for (const p of [4, 8, 10, 12, 14, 16]) {
        const m = 1 << p;
        const h = new HyperLogLog(p);
        const truth = 20000;
        for (let i = 0; i < truth; i++) h.add("p" + p + ":" + i);
        eq(h.precision, p, "precision " + p + " reported");
        eq(h.registers, m, "registers for precision " + p);
        within(h.count(), truth, m, 4, "p=" + p);
    }
}

{
    const h = new HyperLogLog(14);
    const zero = h.count();
    check(zero === 0, "an empty sketch counts 0, got " + zero);
    check(Number.isFinite(zero), "and it is finite, not NaN");

    for (const k of [1, 2, 3, 10, 100, 1000]) {
        const s = new HyperLogLog(14);
        for (let i = 0; i < k; i++) s.add("s" + i);
        const c = s.count();
        check(Number.isFinite(c), k + " keys gives a finite estimate, got " + c);
        within(c, k, 1 << 14, 4, "linear-counting range, k=" + k);
    }

    const one = new HyperLogLog(14);
    one.add("only");
    const c1 = one.count();
    one.add("second");
    check(one.count() > c1,
          "one key -> two keys moves the estimate (" + c1 + " -> " +
          one.count() + ")");
}

{
    const h = new HyperLogLog(4);
    eq(h.registers, 16, "the smallest sketch has 16 registers");
    for (let i = 0; i < 500; i++) h.add("t" + i);
    check(Number.isFinite(h.count()) && h.count() > 0,
          "precision 4 still produces a finite positive estimate");

    for (let p = 4; p <= 18; p++) {
        const s = new HyperLogLog(p);
        eq(s.registers % 8, 0, "precision " + p + " registers divide 8 chains");
    }
}

{
    const a = new HyperLogLog(10), b = new HyperLogLog(12);
    for (let i = 0; i < 100; i++) { a.add("x" + i); b.add("y" + i); }
    const before = a.count();
    let threw = false;
    try { a.merge(b); } catch (e) { threw = true; }
    check(threw, "merging different precisions is refused");
    eq(a.count(), before, "a refused merge leaves the estimate alone");
}

if (fails === 0) print("test_structures_hll: all " + n + " checks passed");
else print("test_structures_hll: " + fails + " FAILED of " + n);
