/* test_fn_timers.js -- Function.prototype debounce/throttle/delay/memoize
 * (STDLIB_OOP_PLAN section 13, "function timers").
 *
 * `lazy` is deliberately absent: a lazily-initialised value is
 * `(() => expensive()).once()`, which already computes at most once and
 * returns the same result forever. A second spelling of one behaviour is a
 * second thing to keep correct.
 *
 * TIMING IS ASSERTED AS ORDER, NEVER AS DURATION. Every check below is of the
 * form "these calls happened, in this sequence"; nothing asserts that a delay
 * took a particular number of milliseconds, because a test that does is a test
 * that fails on a loaded machine for a reason that is not a bug. The waits are
 * generous multiples of the intervals for the same reason.
 *
 * Run: dynajs tests/test_fn_timers.js
 */
import * as os from "os";

let n = 0, failures = 0;
function assert(c, m) {
    n++;
    if (!c) { failures++; print("FAIL: " + m); }
}
function eq(got, want, m) {
    assert(got === want, m + " (got " + JSON.stringify(got) +
           ", want " + JSON.stringify(want) + ")");
}
function throws(fn, kind, m) {
    n++;
    try { fn(); } catch (e) {
        if (kind && !(e instanceof kind)) { failures++; print("FAIL: " + m + " -> " + e); }
        return String(e.message || e);
    }
    failures++;
    print("FAIL: " + m + " did not throw");
    return "";
}

/* ==================================================================== *
 *  memoize -- no scheduler involved, so it is all synchronous
 * ==================================================================== */
{
    let calls = 0;
    const double = x => { calls++; return x * 2; };
    const m = double.memoize();
    eq(m(3), 6, "memoize returns the value");
    eq(m(3), 6, "and the same value again");
    eq(calls, 1, "having called the function once");
    eq(m(4), 8, "a new key computes");
    eq(calls, 2, "once");

    /* The key is a Map key, so it is SameValueZero and not a string: 1 and "1"
     * are different arguments and must not share a result. */
    const idc = { c: 0 };
    const id = x => { idc.c++; return typeof x; };
    const mi = id.memoize();
    eq(mi(1), "number", "number key");
    eq(mi("1"), "string", "string key is a DIFFERENT key");
    eq(idc.c, 2, "so both were computed");
    /* NaN is a legal Map key and equals itself there, unlike ===. */
    eq(mi(NaN), "number", "NaN keys");
    eq(mi(NaN), "number", "and hits the cache");
    eq(idc.c, 3, "NaN is one key, not two");
    /* And so are objects, by identity. */
    const a = {}, b = {};
    const oc = { c: 0 };
    const obj = o => { oc.c++; return o; };
    const mo = obj.memoize();
    assert(mo(a) === a && mo(a) === a && mo(b) === b, "object keys by identity");
    eq(oc.c, 2, "two distinct objects, two calls");

    /* More than one argument with no key function is refused, because any
     * single-argument key for a two-argument call is a silent collision. */
    const msg = throws(() => double.memoize()(1, 2), TypeError, "multi-arg default key");
    assert(msg.indexOf("key function") >= 0, "and the error says what to do: " + msg);

    let kc = 0;
    const add = (x, y) => { kc++; return x + y; };
    const km = add.memoize((x, y) => x + "|" + y);
    eq(km(1, 2), 3, "keyed memo");
    eq(km(1, 2), 3, "cache hit");
    eq(km(2, 1), 3, "a different key with the same answer still computes");
    eq(kc, 2, "two distinct keys");

    /* A throwing call is NOT cached: the next call gets to try again. */
    let tc = 0;
    const flaky = x => { tc++; if (tc < 2) throw new Error("first"); return x; };
    const fm = flaky.memoize();
    let threw = false;
    try { fm(1); } catch (e) { threw = true; }
    assert(threw, "the first call throws");
    eq(fm(1), 1, "and the second succeeds");
    eq(tc, 2, "because the failure was not cached");

    /* `this` reaches the underlying function. */
    const holder = { v: 7, get() { return this.v; } };
    holder.mget = holder.get.memoize(function () { return this.v; });
    eq(holder.mget(), 7, "the receiver is forwarded");

    /* A cached `undefined` is a HIT, not a miss. This is the edge the lookup
     * order creates: get() alone cannot tell "absent" from "present and
     * undefined", so has() is consulted exactly when get() came back
     * undefined. A memo that recomputed here would be calling the function
     * again forever for any function that returns nothing. */
    let uc = 0;
    const nothing = () => { uc++; return undefined; };
    const mn = nothing.memoize();
    mn(1); mn(1); mn(1);
    eq(uc, 1, "a cached undefined is a hit");
    eq(mn(1), undefined, "and it still returns undefined");
    mn(2);
    eq(uc, 2, "a different key still computes");

    /* Two memos of the same function have independent caches. */
    let sc = 0;
    const s = x => { sc++; return x; };
    const m1 = s.memoize(), m2 = s.memoize();
    m1(1); m2(1);
    eq(sc, 2, "each wrapper has its own cache");
}

/* ==================================================================== *
 *  debounce / throttle / delay
 * ==================================================================== */
const log = [];
const rec = tag => { log.push(tag); return tag; };

/* debounce: only the LAST call in a burst runs. */
const d = rec.debounce(20);
d("d-first"); d("d-middle"); d("d-last");

/* throttle: the first call runs immediately, the last of the burst runs at the
 * trailing edge, and the ones between are superseded. */
const t = rec.throttle(30);
const leading = t("t-first");
t("t-middle");
t("t-last");
eq(leading, "t-first", "throttle runs the leading call and returns its result");
eq(log.length, 1, "and nothing else has run yet");
eq(log[0], "t-first", "the leading call is the one that ran");

/* delay: one scheduled call, and a cancelled one that must never appear. */
rec.delay(10, "delayed");
const doomed = rec.delay(10, "cancelled-delay");
doomed.cancel();
assert(typeof doomed.cancel === "function", "delay returns a handle with cancel");

/* a debounce that is cancelled before it fires */
const c = rec.debounce(15);
c("cancelled-debounce");
c.cancel();

/* cancelling twice, and cancelling nothing, are both no-ops rather than errors */
c.cancel();
rec.debounce(5).cancel();

/* flush runs the pending call NOW and returns its result */
const f = rec.debounce(100000);
f("flushed");
eq(f.flush(), "flushed", "flush runs the pending call and returns its value");
/* ...and having flushed, there is nothing left to fire */
eq(f.flush(), undefined, "a second flush has nothing to run");

/* Argument and receiver forwarding through the scheduler. */
const seen = [];
const many = function (...args) { seen.push([this && this.tag, args]); };
const obj = { tag: "obj" };
obj.d = many.debounce(20);
obj.d(1, 2, 3);

os.setTimeout(() => {
    /* Everything scheduled above has now had 120 ms to run against intervals of
     * 10-30 ms. What is asserted is WHICH calls happened, not when. */
    eq(log.indexOf("d-first"), -1, "debounce dropped the first of the burst");
    eq(log.indexOf("d-middle"), -1, "and the middle one");
    assert(log.indexOf("d-last") >= 0, "and ran the last one");
    assert(log.indexOf("t-last") >= 0, "throttle ran the trailing call");
    eq(log.indexOf("t-middle"), -1, "and superseded the one before it");
    assert(log.indexOf("delayed") >= 0, "delay ran");
    eq(log.indexOf("cancelled-delay"), -1, "a cancelled delay never ran");
    eq(log.indexOf("cancelled-debounce"), -1, "a cancelled debounce never ran");
    assert(log.indexOf("flushed") >= 0, "the flushed call ran");
    /* The leading throttle call is still first: order is preserved. */
    eq(log[0], "t-first", "the leading throttle call stayed first");

    eq(seen.length, 1, "the debounced method ran once");
    eq(seen[0][0], "obj", "with its receiver");
    eq(JSON.stringify(seen[0][1]), "[1,2,3]", "and all its arguments");

    /* A second burst after the window fires again -- the wrapper is reusable,
     * not one-shot. */
    const again = [];
    const r2 = (x => again.push(x)).debounce(10);
    r2("again-1"); r2("again-2");
    os.setTimeout(() => {
        eq(JSON.stringify(again), '["again-2"]', "the wrapper is reusable");

        /* Bad intervals are refused rather than silently treated as zero. */
        throws(() => rec.debounce(-1), RangeError, "a negative debounce");
        throws(() => rec.throttle(-1), RangeError, "a negative throttle");
        throws(() => rec.delay(-1), RangeError, "a negative delay");
        throws(() => rec.debounce(NaN), RangeError, "a NaN debounce");

        /* Zero is legal and means "the next turn of the loop". */
        const z = [];
        const zf = (x => z.push(x)).debounce(0);
        zf("zero");
        os.setTimeout(() => {
            eq(JSON.stringify(z), '["zero"]', "a zero debounce still defers");
            if (failures)
                throw new Error("test_fn_timers: " + failures + " failures");
            print("test_fn_timers: all " + n + " assertions passed");
        }, 30);
    }, 60);
}, 120);
