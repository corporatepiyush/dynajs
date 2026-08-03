/*
 * HyperLogLog: the cached estimate and the reassociated register sum.
 *
 * count() is O(m), so it caches and every writer of a register has to
 * invalidate. There are THREE such writers -- add(), merge(), and the raw
 * pointer the deserializer fills through -- and each fails the same silent way:
 * a populated sketch keeps answering a stale number. Nothing crashes and no
 * round trip notices, because the wrong value is a perfectly good double.
 *
 * So each writer gets a case that ONLY it can satisfy. Injecting the missing
 * invalidation at one writer must fail exactly that case; if removing it fails
 * nothing, the case is not testing what it claims.
 *
 * The accuracy assertions are bounds, never equalities: the sum is reassociated
 * across 8 accumulator chains, and pinning digits of a floating-point sum is
 * not portable across targets that contract differently.
 */
import { HyperLogLog } from "dyna:structures";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(a === b, m + " -- got " + a + ", want " + b); }

/* The standard error of an HLL is ~1.04/sqrt(m). Three sigma is the honest
   bound for a single trial; anything tighter fails for statistical reasons
   rather than for bugs, which is the "assert durations" mistake in disguise. */
function within(est, truth, m, sigmas, label) {
    const err = 1.04 / Math.sqrt(m);
    const lo = truth * (1 - sigmas * err) - 1, hi = truth * (1 + sigmas * err) + 1;
    check(est >= lo && est <= hi,
          label + ": " + est.toFixed(0) + " outside [" + lo.toFixed(0) + ", " +
          hi.toFixed(0) + "] for a true " + truth);
}

/* ------------------------------------------- 1. add() invalidates
   The estimate must MOVE after adding. If add forgot to invalidate, count
   would keep returning the value from before the loop. */
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

    /* Adding a key that is ALREADY present must not move it. This is the other
       half: an over-eager invalidation is not a bug, but a cache that returns
       a different value for an unchanged sketch is. */
    const stable = h.count();
    for (let i = 0; i < 1000; i++) h.add("b" + i);      /* all duplicates */
    eq(h.count(), stable, "duplicates do not move the estimate");
}

/* --------------------------------------- 2. merge() invalidates
   ONLY merge can satisfy this: the sketch is counted first (populating the
   cache), then changed exclusively through merge. If merge does not
   invalidate, count returns the pre-merge number. */
{
    const a = new HyperLogLog(14), b = new HyperLogLog(14);
    for (let i = 0; i < 5000; i++) a.add("m" + i);
    for (let i = 5000; i < 60000; i++) b.add("m" + i);

    const beforeMerge = a.count();          /* populates the cache */
    within(beforeMerge, 5000, 1 << 14, 3, "before merge");

    a.merge(b);
    const afterMerge = a.count();
    check(afterMerge > beforeMerge * 5,
          "merge() invalidates: " + beforeMerge.toFixed(0) + " -> " +
          afterMerge.toFixed(0) + " (a stale cache returns the first number)");
    within(afterMerge, 60000, 1 << 14, 3, "merged union");

    /* Merging a sketch that adds nothing new must not move it. */
    const settled = a.count();
    const c = new HyperLogLog(14);
    for (let i = 0; i < 100; i++) c.add("m" + i);       /* a subset */
    a.merge(c);
    eq(a.count(), settled, "merging a subset does not move the estimate");
}

/* ------------------------------- 3. deserialize invalidates
   The deserializer memcpys straight into the register array. A fresh sketch
   has a zeroed cache, so if that path does not invalidate, EVERY decoded
   sketch reports 0 -- the loudest of the three failures and the easiest to
   ship, because serialize/deserialize round trips still "work". */
{
    const src = new HyperLogLog(14);
    for (let i = 0; i < 50000; i++) src.add("d" + i);
    const want = src.count();

    const back = HyperLogLog.deserialize(src.serialize());
    const got = back.count();               /* the FIRST call on a fresh object */
    check(got > 0, "a decoded sketch does not report zero (got " + got + ")");
    eq(got, want, "a decoded sketch reports exactly the source's estimate");

    /* And it must keep working afterwards, not just answer once. */
    eq(back.count(), want, "a decoded sketch is stable across calls");
    back.add("brand new key that cannot be present");
    check(back.count() >= want, "a decoded sketch still accepts adds");
}

/* --------------------------------- 4. accuracy across precisions and sizes
   The error bound is a function of m, so a single precision proves one point
   of a curve. Small m must be visibly worse than large m -- if they are the
   same, the precision argument is being ignored. */
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

/* ----------------------------------------- 5. the small-cardinality range
   Below 2.5m the estimator switches to linear counting, which is a different
   branch. Zero is its edge: an empty sketch has no zero-register term to take
   a log of, and must not produce NaN or Infinity. */
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

    /* Exactly one key, counted before and after a second distinct key: the
       smallest possible invalidation. */
    const one = new HyperLogLog(14);
    one.add("only");
    const c1 = one.count();
    one.add("second");
    check(one.count() > c1,
          "one key -> two keys moves the estimate (" + c1 + " -> " +
          one.count() + ")");
}

/* --------------------------------- 6. the tail of the accumulator loop
   The register count is a power of two and 8 chains divide every one of them,
   so the scalar tail never runs at any supported precision. Precision 4 is
   the smallest -- 16 registers, exactly two full chains -- and is the case
   that would break first if the bound were ever computed wrong. */
{
    const h = new HyperLogLog(4);
    eq(h.registers, 16, "the smallest sketch has 16 registers");
    for (let i = 0; i < 500; i++) h.add("t" + i);
    check(Number.isFinite(h.count()) && h.count() > 0,
          "precision 4 still produces a finite positive estimate");

    /* Every supported precision must have a register count divisible by 8, or
       the tail loop is reachable and untested. */
    for (let p = 4; p <= 18; p++) {
        const s = new HyperLogLog(p);
        eq(s.registers % 8, 0, "precision " + p + " registers divide 8 chains");
    }
}

/* --------------------------------- 7. merge refuses a precision mismatch */
{
    const a = new HyperLogLog(10), b = new HyperLogLog(12);
    for (let i = 0; i < 100; i++) { a.add("x" + i); b.add("y" + i); }
    const before = a.count();
    let threw = false;
    try { a.merge(b); } catch (e) { threw = true; }
    check(threw, "merging different precisions is refused");
    /* And a refused merge must not have disturbed the estimate. */
    eq(a.count(), before, "a refused merge leaves the estimate alone");
}

if (fails === 0) print("test_structures_hll: all " + n + " checks passed");
else print("test_structures_hll: " + fails + " FAILED of " + n);
