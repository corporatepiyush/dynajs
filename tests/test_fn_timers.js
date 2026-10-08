// flags: --std
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

{
    let calls = 0;
    const double = x => { calls++; return x * 2; };
    const m = double.memoize();
    eq(m(3), 6, "memoize returns the value");
    eq(m(3), 6, "and the same value again");
    eq(calls, 1, "having called the function once");
    eq(m(4), 8, "a new key computes");
    eq(calls, 2, "once");

    const idc = { c: 0 };
    const id = x => { idc.c++; return typeof x; };
    const mi = id.memoize();
    eq(mi(1), "number", "number key");
    eq(mi("1"), "string", "string key is a DIFFERENT key");
    eq(idc.c, 2, "so both were computed");
    eq(mi(NaN), "number", "NaN keys");
    eq(mi(NaN), "number", "and hits the cache");
    eq(idc.c, 3, "NaN is one key, not two");
    const a = {}, b = {};
    const oc = { c: 0 };
    const obj = o => { oc.c++; return o; };
    const mo = obj.memoize();
    assert(mo(a) === a && mo(a) === a && mo(b) === b, "object keys by identity");
    eq(oc.c, 2, "two distinct objects, two calls");

    const msg = throws(() => double.memoize()(1, 2), TypeError, "multi-arg default key");
    assert(msg.indexOf("key function") >= 0, "and the error says what to do: " + msg);

    let kc = 0;
    const add = (x, y) => { kc++; return x + y; };
    const km = add.memoize((x, y) => x + "|" + y);
    eq(km(1, 2), 3, "keyed memo");
    eq(km(1, 2), 3, "cache hit");
    eq(km(2, 1), 3, "a different key with the same answer still computes");
    eq(kc, 2, "two distinct keys");

    let tc = 0;
    const flaky = x => { tc++; if (tc < 2) throw new Error("first"); return x; };
    const fm = flaky.memoize();
    let threw = false;
    try { fm(1); } catch (e) { threw = true; }
    assert(threw, "the first call throws");
    eq(fm(1), 1, "and the second succeeds");
    eq(tc, 2, "because the failure was not cached");

    const holder = { v: 7, get() { return this.v; } };
    holder.mget = holder.get.memoize(function () { return this.v; });
    eq(holder.mget(), 7, "the receiver is forwarded");

    let uc = 0;
    const nothing = () => { uc++; return undefined; };
    const mn = nothing.memoize();
    mn(1); mn(1); mn(1);
    eq(uc, 1, "a cached undefined is a hit");
    eq(mn(1), undefined, "and it still returns undefined");
    mn(2);
    eq(uc, 2, "a different key still computes");

    let sc = 0;
    const s = x => { sc++; return x; };
    const m1 = s.memoize(), m2 = s.memoize();
    m1(1); m2(1);
    eq(sc, 2, "each wrapper has its own cache");
}

const log = [];
const rec = tag => { log.push(tag); return tag; };

const d = rec.debounce(20);
d("d-first"); d("d-middle"); d("d-last");

const t = rec.throttle(30);
const leading = t("t-first");
t("t-middle");
t("t-last");
eq(leading, "t-first", "throttle runs the leading call and returns its result");
eq(log.length, 1, "and nothing else has run yet");
eq(log[0], "t-first", "the leading call is the one that ran");

rec.delay(10, "delayed");
const doomed = rec.delay(10, "cancelled-delay");
doomed.cancel();
assert(typeof doomed.cancel === "function", "delay returns a handle with cancel");

const c = rec.debounce(15);
c("cancelled-debounce");
c.cancel();

c.cancel();
rec.debounce(5).cancel();

const f = rec.debounce(100000);
f("flushed");
eq(f.flush(), "flushed", "flush runs the pending call and returns its value");
eq(f.flush(), undefined, "a second flush has nothing to run");

const seen = [];
const many = function (...args) { seen.push([this && this.tag, args]); };
const obj = { tag: "obj" };
obj.d = many.debounce(20);
obj.d(1, 2, 3);

os.setTimeout(() => {
    eq(log.indexOf("d-first"), -1, "debounce dropped the first of the burst");
    eq(log.indexOf("d-middle"), -1, "and the middle one");
    assert(log.indexOf("d-last") >= 0, "and ran the last one");
    assert(log.indexOf("t-last") >= 0, "throttle ran the trailing call");
    eq(log.indexOf("t-middle"), -1, "and superseded the one before it");
    assert(log.indexOf("delayed") >= 0, "delay ran");
    eq(log.indexOf("cancelled-delay"), -1, "a cancelled delay never ran");
    eq(log.indexOf("cancelled-debounce"), -1, "a cancelled debounce never ran");
    assert(log.indexOf("flushed") >= 0, "the flushed call ran");
    eq(log[0], "t-first", "the leading throttle call stayed first");

    eq(seen.length, 1, "the debounced method ran once");
    eq(seen[0][0], "obj", "with its receiver");
    eq(JSON.stringify(seen[0][1]), "[1,2,3]", "and all its arguments");

    const again = [];
    const r2 = (x => again.push(x)).debounce(10);
    r2("again-1"); r2("again-2");
    os.setTimeout(() => {
        eq(JSON.stringify(again), '["again-2"]', "the wrapper is reusable");

        throws(() => rec.debounce(-1), RangeError, "a negative debounce");
        throws(() => rec.throttle(-1), RangeError, "a negative throttle");
        throws(() => rec.delay(-1), RangeError, "a negative delay");
        throws(() => rec.debounce(NaN), RangeError, "a NaN debounce");

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
