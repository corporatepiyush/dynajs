/*
 * IntervalTree's unindexed tail.
 *
 * The sorted array plus max-hi segment tree is rebuilt lazily, and rebuilding
 * on every query made an interleaved insert/query workload O(n) per call --
 * 38,125 ns at n=8000. A short tail of recent inserts is now left OUT of the
 * index and scanned linearly instead, which is 700 ns and flat.
 *
 * That splits every query into two paths that must agree:
 *
 *   indexed    the binary search plus tree descent over v[0..built)
 *   pending    a linear scan over v[built..len)
 *
 * A result found by one and missed by the other is a silently wrong answer,
 * not a crash, and it only appears when a query follows an insert closely
 * enough that the rebuild threshold has not been crossed. So the oracle here
 * is a JS model of the same intervals, checked after EVERY insert rather than
 * once at the end -- checking only at the end always rebuilds first and would
 * never execute the pending path at all.
 */
import { IntervalTree } from "dyna:structures";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + a + ", want " + b); }

/* The oracle: every interval, scanned. Results are compared as SETS, because
   the two paths yield them in different orders by construction. */
function model() {
    const iv = [];
    return {
        add(lo, hi, v) { iv.push([lo, hi, v]); },
        del(v) { const i = iv.findIndex(x => x[2] === v);
                 if (i < 0) return false; iv.splice(i, 1); return true; },
        at(x) { return iv.filter(r => r[0] <= x && r[1] >= x).map(r => r[2]).sort((a,b)=>a-b); },
        over(lo, hi) { return iv.filter(r => r[0] <= hi && r[1] >= lo).map(r => r[2]).sort((a,b)=>a-b); },
        size() { return iv.length; },
    };
}
/* at()/overlapping() yield [lo, hi, value] triples; only the value identifies
   the interval, and the two paths return them in different orders. */
const norm = (a) => a.map(r => r[2]).sort((x, y) => x - y);
function same(got, want) {
    const g = norm(got);
    if (g.length !== want.length) return "length " + g.length + " vs " + want.length;
    for (let i = 0; i < g.length; i++)
        if (g[i] !== want[i]) return "at " + i + ": " + g[i] + " vs " + want[i];
    return null;
}

/* ------------------- 1. query after EVERY insert, so the tail is never empty
   This is the case the old code could not get wrong, because it rebuilt every
   time. It is now the only case that runs the pending scan. */
{
    const t = new IntervalTree(), m = model();
    let bad = null;
    for (let i = 0; i < 400 && !bad; i++) {
        const lo = (i * 37) % 500, hi = lo + 20 + (i % 15);
        t.insert(lo, hi, i); m.add(lo, hi, i);
        /* probe points that straddle the newest interval and older ones */
        for (const x of [lo, lo + 5, hi, hi + 1, (i * 11) % 520]) {
            bad = same(t.at(x), m.at(x));
            if (bad) { bad = "at(" + x + ") after insert " + i + ": " + bad; break; }
        }
        if (!bad) {
            const a = (i * 7) % 400, b = a + 60;
            bad = same(t.overlapping(a, b), m.over(a, b));
            if (bad) bad = "overlapping(" + a + "," + b + ") after insert " + i + ": " + bad;
        }
    }
    check(bad === null, "insert-then-query agrees with a full scan -- " + bad);
    eq(t.size, m.size(), "size after interleaving");
}

/* ------- 2. the newest interval must be findable IMMEDIATELY after insert
   If the pending scan were skipped, a just-inserted interval would be
   invisible until the next rebuild -- which a bulk-then-query test cannot see. */
{
    const t = new IntervalTree();
    for (let i = 0; i < 200; i++) t.insert(i * 10, i * 10 + 5, i);
    t.at(0);                                   /* force a build */
    for (let i = 0; i < 40; i++) {
        const lo = 100000 + i * 3;
        t.insert(lo, lo + 1, 9000 + i);
        const got = norm(t.at(lo));
        check(got.indexOf(9000 + i) >= 0,
              "an interval inserted after the build is found immediately (i=" + i + ")");
    }
    /* and every one of them survives the eventual rebuild */
    for (let i = 0; i < 2000; i++) t.insert(-1 - i, -1, 50000 + i);
    let missing = -1;
    for (let i = 0; i < 40; i++) {
        const lo = 100000 + i * 3;
        if (norm(t.at(lo)).indexOf(9000 + i) < 0) { missing = i; break; }
    }
    check(missing < 0, "and survives the rebuild, lost i=" + missing);
}

/* ---------------- 3. the rebuild THRESHOLD, crossed in both directions
   The tail is capped at 16 + built/8, so a burst larger than that triggers a
   rebuild mid-sequence. Both sides of the crossing must answer identically. */
{
    const t = new IntervalTree(), m = model();
    for (let i = 0; i < 64; i++) { t.insert(i, i + 3, i); m.add(i, i + 3, i); }
    t.at(0);                                   /* built = 64, cap = 16 + 8 */
    let bad = null;
    for (let burst = 1; burst <= 40 && !bad; burst++) {
        const base = 1000 + burst * 100;
        for (let k = 0; k < burst; k++) { t.insert(base + k, base + k + 2, base + k);
                                          m.add(base + k, base + k + 2, base + k); }
        for (const x of [base, base + burst, 5, 40]) {
            bad = same(t.at(x), m.at(x));
            if (bad) { bad = "burst " + burst + " at(" + x + "): " + bad; break; }
        }
    }
    check(bad === null, "answers match across the rebuild threshold -- " + bad);
    eq(t.size, m.size(), "size across bursts");
}

/* --------- 4. removal is NOT reachable from JS
   The core has dyn_itree_remove_at and it drops the whole index, because a
   swap-remove puts an unsorted element into the sorted prefix. No method
   exposes it, so that branch is UNREACHED rather than tested -- recorded here
   so the next reader does not count it as a verified path. */
{
    const t = new IntervalTree();
    t.insert(1, 2, 0);
    check(typeof t.removeAt !== "function" && typeof t.delete !== "function",
          "IntervalTree exposes no removal, so the index-drop branch is unreached");
}

/* --------------------------------- 5. a record round trip keeps every result */
{
    const t = new IntervalTree(), m = model();
    for (let i = 0; i < 500; i++) { const lo = (i * 91) % 2000;
        t.insert(lo, lo + 30, i); m.add(lo, lo + 30, i); }
    /* leave a pending tail unindexed at serialize time */
    t.at(0);
    for (let i = 500; i < 510; i++) { t.insert(i, i + 5, i); m.add(i, i + 5, i); }
    const back = IntervalTree.deserialize(t.serialize());
    eq(back.size, t.size, "decoded size");
    let bad = null;
    for (let x = 0; x < 600 && !bad; x += 7) bad = same(back.at(x), m.at(x));
    check(bad === null, "a decoded tree answers identically -- " + bad);
}

if (fails === 0) print("test_structures_itree_pending: all " + n + " checks passed");
else print("test_structures_itree_pending: " + fails + " FAILED of " + n);
