/*
 * Heap without a comparator: natural (numeric) order, compared in C.
 *
 * A comparator used to be mandatory, so every sift level paid a full JS call
 * -- three dups, an invocation, a ToFloat64 and three frees, about seventeen
 * times per pushPop on a 100k heap. `new Heap()` now compares numbers
 * natively, which is a new capability and the fast path at once.
 *
 * The two arms are written as SEPARATE loops, so this file has to prove they
 * agree rather than assume it: the oracle throughout is a differential against
 * a heap built with (a, b) => a - b over the same input. A heap that pops one
 * element out of order is still a heap by size and by peek.
 *
 * The natural arm runs no JS at all, which is exactly why it may not coerce:
 * an object with valueOf must be REFUSED, not called. Calling it would run
 * user code inside a sift that has dropped the reentrancy guard precisely
 * because nothing can run.
 */
import { Heap } from "dyna:structures";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + a + ", want " + b); }

/* ------------------------------------------- 1. it is a min-heap at all */
{
    const h = new Heap();
    for (const v of [5, 1, 9, 3, 7, 2, 8]) h.push(v);
    eq(h.size, 7, "size after 7 pushes");
    eq(h.peek(), 1, "peek is the minimum");
    const out = [];
    while (h.size) out.push(h.pop());
    eq(out.join(","), "1,2,3,5,7,8,9", "pops in ascending order");
    eq(h.size, 0, "empty after draining");
    eq(h.pop(), undefined, "popping an empty heap");
    eq(h.peek(), undefined, "peeking an empty heap");
}

/* ---------------------------------- 2. DIFFERENTIAL against a comparator
   The two arms are different loops, so the only honest check is that they
   produce the same sequence on the same input. Deterministic input, because
   Math.random would make a divergence unreproducible. */
{
    for (const N of [1, 2, 3, 17, 64, 65, 1000, 20000]) {
        const a = new Heap((x, y) => x - y), b = new Heap();
        for (let i = 0; i < N; i++) {
            const v = ((i * 2654435761) % 1000003) - 500000;
            a.push(v); b.push(v);
        }
        eq(b.size, a.size, "N=" + N + ": size");
        let bad = -1;
        for (let i = 0; i < N; i++) {
            const x = a.pop(), y = b.pop();
            if (!Object.is(x, y)) { bad = i; break; }
        }
        check(bad < 0, "N=" + N + ": diverges at pop " + bad);
    }

    /* Interleaved push and pop, which drives sift_up and sift_down in the
       same run rather than one phase after the other. */
    const a = new Heap((x, y) => x - y), b = new Heap();
    let bad = -1;
    for (let i = 0; i < 20000; i++) {
        const v = (i * 48271) % 65537;
        a.push(v); b.push(v);
        if (i % 3 === 0) {
            const x = a.pop(), y = b.pop();
            if (!Object.is(x, y)) { bad = i; break; }
        }
    }
    check(bad < 0, "interleaved push/pop diverges at " + bad);
    eq(b.size, a.size, "interleaved: final size");
}

/* ------------------------------------ 3. the numeric values that are edges
   Every one of these is a number, so every one must be ACCEPTED, and the
   ordering must match the comparator arm exactly -- including the two that
   are not ordered by < at all. */
{
    const vals = [0, -0, 1, -1, 0.5, -0.5, Infinity, -Infinity,
                  Number.MAX_VALUE, -Number.MAX_VALUE, Number.MIN_VALUE,
                  9007199254740993, -9007199254740993, 1e-300, NaN];
    const a = new Heap((x, y) => x - y), b = new Heap();
    for (const v of vals) { a.push(v); b.push(v); }
    eq(b.size, a.size, "edge values: size");
    let bad = -1;
    for (let i = 0; i < vals.length; i++) {
        const x = a.pop(), y = b.pop();
        if (!Object.is(x, y)) { bad = i; break; }
    }
    check(bad < 0, "edge values diverge at pop " + bad);

    /* -Infinity is the minimum of that set and must come out first. */
    const c = new Heap();
    for (const v of vals) c.push(v);
    eq(c.pop(), -Infinity, "-Infinity is the minimum");

    /* NaN compares equal to everything in BOTH arms, so it does not reorder
       the rest. This is a documented tie, not an accident. */
    const d = new Heap();
    d.push(NaN); d.push(5); d.push(3);
    eq(d.size, 3, "a NaN can be stored");
    const got = [d.pop(), d.pop(), d.pop()];
    check(got.filter(Number.isNaN).length === 1, "the NaN comes back out");
    check(got.filter(v => v === 3).length === 1 &&
          got.filter(v => v === 5).length === 1, "and so do the numbers");
}

/* -------------------------------- 4. non-numbers are REFUSED, not ordered
   The refusal has to happen on the push that first COMPARES the value: a
   single push into an empty heap never compares anything. */
{
    const nonNumbers = [
        ["string", "5"],
        ["boolean", true],
        ["null", null],
        ["undefined", undefined],
        ["object", {}],
        ["array", [1]],
        ["function", function () {}],
    ];
    for (const [label, v] of nonNumbers) {
        const h = new Heap();
        h.push(1);                       /* one number, nothing compared yet */
        let threw = false, msg = "";
        try { h.push(v); } catch (e) { threw = true; msg = e.message; }
        check(threw, "pushing a " + label + " into a natural heap is refused");
        check(threw && /comparator/.test(msg),
              "and the message names the fix -- got " + JSON.stringify(msg));
    }

    /* THE COERCION HAZARD. An object with valueOf must be refused WITHOUT
       calling it: the natural sift drops the reentrancy guard precisely
       because it runs no user code, so a coercion there would run JS inside
       a loop holding a raw pointer into items[]. */
    {
        let called = 0;
        const evil = { valueOf() { called++; return 0; } };
        const h = new Heap();
        h.push(1);
        let threw = false;
        try { h.push(evil); } catch (e) { threw = true; }
        check(threw, "an object with valueOf is refused");
        eq(called, 0, "and its valueOf was NEVER called");
    }

    /* A string heap is a perfectly reasonable thing to want; it just needs a
       comparator, and with one it must work. */
    {
        const h = new Heap((a, b) => a < b ? -1 : (a > b ? 1 : 0));
        for (const s of ["pear", "apple", "fig"]) h.push(s);
        eq(h.pop(), "apple", "a string heap works with a comparator");
    }
}

/* -------------------------------- 5. the constructor's argument handling */
{
    check((() => { try { new Heap(); return true; } catch (e) { return false; } })(),
          "new Heap() is allowed");
    check((() => { try { new Heap(undefined); return true; } catch (e) { return false; } })(),
          "new Heap(undefined) selects natural order");
    /* An explicit non-function is still a mistake and must still throw --
       making the argument optional must not make it permissive. */
    for (const bad of [null, 0, 1, "min", {}, []]) {
        let threw = false;
        try { new Heap(bad); } catch (e) { threw = true; }
        check(threw, "new Heap(" + JSON.stringify(bad) + ") is refused");
    }
    /* and natural order really is what new Heap(undefined) picked */
    const h = new Heap(undefined);
    h.push(3); h.push(1);
    eq(h.pop(), 1, "new Heap(undefined) orders ascending");
}

/* ------------------------------------------ 6. records round-trip both ways */
{
    const a = new Heap();
    for (let i = 0; i < 500; i++) a.push((i * 7919) % 1009);
    const back = Heap.deserialize(a.serialize());
    eq(back.size, a.size, "natural heap: decoded size");
    let bad = -1;
    for (let i = 0; i < 500; i++) {
        const x = a.pop(), y = back.pop();
        if (!Object.is(x, y)) { bad = i; break; }
    }
    check(bad < 0, "a decoded natural heap pops identically, diverged at " + bad);

    /* A comparator heap still requires its comparator, and a decoded one must
       use the SUPPLIED order, not the natural one -- a max-heap decoded
       without its comparator would silently become a min-heap. */
    const m = new Heap((x, y) => y - x);
    for (const v of [5, 1, 9]) m.push(v);
    const md = Heap.deserialize(m.serialize(), (x, y) => y - x);
    eq(md.pop(), 9, "a decoded max-heap is still a max-heap");

    /* Decoding a max-heap's bytes WITHOUT the comparator is legal and gives a
       min-heap: the record carries values, never an order. */
    const mn = Heap.deserialize(m.serialize());
    eq(mn.pop(), 1, "the same bytes decoded naturally are a min-heap");

    /* but a non-function second argument is still a mistake */
    for (const bad of [42, "min", {}]) {
        let threw = false;
        try { Heap.deserialize(m.serialize(), bad); } catch (e) { threw = true; }
        check(threw, "deserialize with " + JSON.stringify(bad) + " is refused");
    }
}

/* ---------------------- 7. the comparator arm keeps its reentrancy guard
   The natural arm dropped `busy` because nothing can re-enter it. The
   comparator arm must NOT have lost it -- a comparator that pushes into the
   heap it is ordering would realloc items[] under the sift. */
{
    const h = new Heap(function (a, b) {
        try { h.push(1); } catch (e) { reentered = e; }
        return a - b;
    });
    let reentered = null;
    h.push(1);
    h.push(2);                            /* forces a comparison */
    check(reentered !== null,
          "a comparator that re-enters push() is still refused");
    /* The MESSAGE, not just that something threw. Without the guard the
       comparator recurses until the stack overflows, which also throws --
       so "it threw" passes with the guard deleted and proves nothing. */
    check(reentered !== null && /must not push\/pop/.test(reentered.message),
          "and it is the reentrancy guard that refused it, not a stack " +
          "overflow -- got " + (reentered && JSON.stringify(reentered.message)));

    /* pop() has its own guard and needs its own case. */
    {
        let caught = null;
        const g = new Heap(function (a, b) {
            try { g.pop(); } catch (e) { caught = e; }
            return a - b;
        });
        g.push(1); g.push(2);
        check(caught !== null && /must not push\/pop/.test(caught.message),
              "a comparator that re-enters pop() is refused by the guard -- " +
              "got " + (caught && JSON.stringify(caught.message)));
    }
}

/* -------------------------------------- 8. a natural heap survives churn
   Enough operations to move the array through several reallocations, with
   the differential still holding at the end. */
{
    const a = new Heap((x, y) => x - y), b = new Heap();
    for (let round = 0; round < 40; round++) {
        for (let i = 0; i < 500; i++) {
            const v = ((round * 500 + i) * 2654435761) % 99991;
            a.push(v); b.push(v);
        }
        for (let i = 0; i < 250; i++) { a.pop(); b.pop(); }
    }
    eq(b.size, a.size, "after churn: size");
    let bad = -1, k = 0;
    while (a.size) {
        const x = a.pop(), y = b.pop();
        if (!Object.is(x, y)) { bad = k; break; }
        k++;
    }
    check(bad < 0, "after churn the sequences still agree, diverged at " + bad);
    check(k > 9000, "the churn really drained " + k + " elements");
}

if (fails === 0) print("test_structures_heap_natural: all " + n + " checks passed");
else print("test_structures_heap_natural: " + fails + " FAILED of " + n);
