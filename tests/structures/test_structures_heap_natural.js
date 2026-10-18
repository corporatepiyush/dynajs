import { Heap } from "dyna:structures";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + a + ", want " + b); }

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

    const c = new Heap();
    for (const v of vals) c.push(v);
    eq(c.pop(), -Infinity, "-Infinity is the minimum");

    const d = new Heap();
    d.push(NaN); d.push(5); d.push(3);
    eq(d.size, 3, "a NaN can be stored");
    const got = [d.pop(), d.pop(), d.pop()];
    check(got.filter(Number.isNaN).length === 1, "the NaN comes back out");
    check(got.filter(v => v === 3).length === 1 &&
          got.filter(v => v === 5).length === 1, "and so do the numbers");
}

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
        h.push(1);
        let threw = false, msg = "";
        try { h.push(v); } catch (e) { threw = true; msg = e.message; }
        check(threw, "pushing a " + label + " into a natural heap is refused");
        check(threw && /comparator/.test(msg),
              "and the message names the fix -- got " + JSON.stringify(msg));
    }

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

    {
        const h = new Heap((a, b) => a < b ? -1 : (a > b ? 1 : 0));
        for (const s of ["pear", "apple", "fig"]) h.push(s);
        eq(h.pop(), "apple", "a string heap works with a comparator");
    }
}

{
    check((() => { try { new Heap(); return true; } catch (e) { return false; } })(),
          "new Heap() is allowed");
    check((() => { try { new Heap(undefined); return true; } catch (e) { return false; } })(),
          "new Heap(undefined) selects natural order");
    for (const bad of [null, 0, 1, "min", {}, []]) {
        let threw = false;
        try { new Heap(bad); } catch (e) { threw = true; }
        check(threw, "new Heap(" + JSON.stringify(bad) + ") is refused");
    }
    const h = new Heap(undefined);
    h.push(3); h.push(1);
    eq(h.pop(), 1, "new Heap(undefined) orders ascending");
}

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

    const m = new Heap((x, y) => y - x);
    for (const v of [5, 1, 9]) m.push(v);
    const md = Heap.deserialize(m.serialize(), (x, y) => y - x);
    eq(md.pop(), 9, "a decoded max-heap is still a max-heap");

    const mn = Heap.deserialize(m.serialize());
    eq(mn.pop(), 1, "the same bytes decoded naturally are a min-heap");

    for (const bad of [42, "min", {}]) {
        let threw = false;
        try { Heap.deserialize(m.serialize(), bad); } catch (e) { threw = true; }
        check(threw, "deserialize with " + JSON.stringify(bad) + " is refused");
    }
}

{
    const h = new Heap(function (a, b) {
        try { h.push(1); } catch (e) { reentered = e; }
        return a - b;
    });
    let reentered = null;
    h.push(1);
    h.push(2);
    check(reentered !== null,
          "a comparator that re-enters push() is still refused");
    check(reentered !== null && /must not push\/pop/.test(reentered.message),
          "and it is the reentrancy guard that refused it, not a stack " +
          "overflow -- got " + (reentered && JSON.stringify(reentered.message)));

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
