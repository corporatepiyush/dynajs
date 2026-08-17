/* test_iterator_gap.js -- the four Iterator members that were missing versus
 * the MDN surface: Iterator.zip, Iterator.zipKeyed,
 * Iterator.prototype.includes and Iterator.prototype[Symbol.dispose].
 *
 * The interesting part of zip is not the happy path, it is what happens when
 * the inputs DISAGREE about when to stop, and whether the inputs that were not
 * exhausted get closed. An unclosed input is how you leak a file handle or hang
 * on an infinite generator, and neither shows up in a "does it produce the
 * right tuples" test -- so most of this file is about closing and about the
 * three modes' boundaries.
 *
 * Run: dynajs tests/test_iterator_gap.js */

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(got, want, m) {
    n++;
    if (got !== want) throw new Error("assertion failed: " + m +
        "\n  got:  " + String(got) + "\n  want: " + String(want));
}
function eqJSON(got, want, m) {
    n++;
    const a = JSON.stringify(got), b = JSON.stringify(want);
    if (a !== b) throw new Error("assertion failed: " + m + "\n  got:  " + a + "\n  want: " + b);
}
function throwsType(fn, m) {
    n++;
    try { fn(); } catch (e) {
        if (e instanceof TypeError) return;
        throw new Error("assertion failed (wrong type): " + m + " -> " + e);
    }
    throw new Error("assertion failed (expected TypeError): " + m);
}

/* A counted iterator: reports how many values it produced and whether it was
 * closed. This is the instrument for every cleanup assertion below. */
function counted(values, opts = {}) {
    const state = { produced: 0, closed: 0 };
    let i = 0;
    state.iter = {
        [Symbol.iterator]() { return this; },
        next() {
            if (opts.infinite) { state.produced++; return { value: state.produced - 1, done: false }; }
            if (i >= values.length) return { value: undefined, done: true };
            state.produced++;
            return { value: values[i++], done: false };
        },
        return() { state.closed++; return { done: true }; },
    };
    return state;
}

/* ==================================================================== *
 *  presence -- the gap this file closes
 * ==================================================================== */
{
    const statics = Object.getOwnPropertyNames(Iterator);
    for (const m of ["zip", "zipKeyed", "concat", "from"])
        assert(statics.includes(m), "Iterator." + m + " exists");
    const proto = Object.getOwnPropertyNames(Iterator.prototype);
    assert(proto.includes("includes"), "Iterator.prototype.includes exists");
    const syms = Object.getOwnPropertySymbols(Iterator.prototype);
    assert(syms.includes(Symbol.dispose), "Iterator.prototype[Symbol.dispose] exists");
    eq(Iterator.zip.length, 1, "Iterator.zip.length");
    eq(Iterator.zipKeyed.length, 1, "Iterator.zipKeyed.length");
    eq(Iterator.prototype.includes.length, 1, "includes.length");
}

/* ==================================================================== *
 *  Iterator.prototype.includes
 * ==================================================================== */
{
    eq([1, 2, 3].values().includes(2), true, "finds a present value");
    eq([1, 2, 3].values().includes(9), false, "misses an absent value");
    eq([].values().includes(1), false, "empty iterator finds nothing");

    /* SameValueZero, not === : this is the entire reason includes exists
     * alongside a find/indexOf style search. */
    eq([NaN].values().includes(NaN), true, "NaN finds NaN (SameValueZero)");
    eq([-0].values().includes(0), true, "-0 matches +0");
    eq([0].values().includes(-0), true, "+0 matches -0");
    eq(["1"].values().includes(1), false, "no type coercion");
    const obj = {};
    eq([obj].values().includes(obj), true, "identity for objects");
    eq([{}].values().includes({}), false, "structurally equal objects are not the same");

    /* a hit CLOSES the iterator (it is abandoned mid-stream); a miss exhausts
     * it, so there is nothing left to close */
    {
        const s = counted([1, 2, 3, 4, 5]);
        eq(Iterator.from(s.iter).includes(3), true, "hit");
        eq(s.closed, 1, "a hit closes the abandoned iterator");
        eq(s.produced, 3, "a hit stops pulling immediately");
    }
    {
        const s = counted([1, 2, 3]);
        eq(Iterator.from(s.iter).includes(99), false, "miss");
        eq(s.closed, 0, "an exhausted iterator is not closed again");
        eq(s.produced, 3, "a miss consumes everything");
    }
    /* it terminates on an infinite source as soon as it matches */
    {
        const s = counted(null, { infinite: true });
        eq(Iterator.from(s.iter).includes(4), true, "finds a value in an infinite source");
        eq(s.closed, 1, "and closes it");
    }
    /* works after other helpers in a chain */
    eq([1, 2, 3, 4].values().map(x => x * 10).includes(30), true, "includes after map");
    eq([1, 2, 3, 4].values().filter(x => x % 2).includes(4), false, "includes after filter");
}

/* ==================================================================== *
 *  Iterator.prototype[Symbol.dispose]
 * ==================================================================== */
{
    const s = counted([1, 2, 3]);
    const it = Iterator.from(s.iter);
    it.next();
    eq(it[Symbol.dispose](), undefined, "dispose returns undefined");
    eq(s.closed, 1, "dispose closes the underlying iterator");

    /* an iterator with no return method disposes harmlessly -- IteratorClose is
     * a no-op there, so `using` over a plain generator is safe */
    eq([1, 2].values()[Symbol.dispose](), undefined, "dispose on an array iterator is a no-op");
    function* gen() { yield 1; }
    eq(gen()[Symbol.dispose](), undefined, "dispose on a generator is safe");

    /* it is callable more than once */
    const s2 = counted([1]);
    const it2 = Iterator.from(s2.iter);
    it2[Symbol.dispose]();
    it2[Symbol.dispose]();
    assert(s2.closed >= 1, "repeated dispose does not throw");
}

/* ==================================================================== *
 *  Iterator.zip -- shapes
 * ==================================================================== */
{
    eqJSON(Iterator.zip([[1, 2, 3], ["a", "b", "c"]]).toArray(),
           [[1, "a"], [2, "b"], [3, "c"]], "equal lengths");
    eqJSON(Iterator.zip([[1, 2, 3, 4], ["a", "b", "c"]]).toArray(),
           [[1, "a"], [2, "b"], [3, "c"]], "shortest truncates");
    eqJSON(Iterator.zip([]).toArray(), [], "no inputs -> immediately done");
    eqJSON(Iterator.zip([[1, 2, 3]]).toArray(), [[1], [2], [3]], "a single input still tuples");
    eqJSON(Iterator.zip([[], [1, 2]]).toArray(), [], "an empty input ends it at once");
    eqJSON(Iterator.zip([[1, 2], [3, 4], [5, 6]]).toArray(),
           [[1, 3, 5], [2, 4, 6]], "three inputs");
    /* tuple width always equals the input count, even when values are absent */
    {
        const r = Iterator.zip([[1, 2], ["a"]], { mode: "longest" }).toArray();
        eq(r[1].length, 2, "a longest tuple keeps full width");
        eq(r[1][1], undefined, "the missing slot is undefined, not absent");
    }
}

/* ==================================================================== *
 *  Iterator.zip -- modes
 * ==================================================================== */
{
    eqJSON(Iterator.zip([[1, 2, 3, 4], ["a", "b", "c"]], { mode: "longest" }).toArray(),
           [[1, "a"], [2, "b"], [3, "c"], [4, null]], "longest pads with undefined");
    eqJSON(Iterator.zip([[1, 2, 3, 4], ["a", "b", "c"]],
                        { mode: "longest", padding: ["X", "Y"] }).toArray(),
           [[1, "a"], [2, "b"], [3, "c"], [4, "Y"]],
           "padding[i] fills input i, not the tuple position");
    /* padding shorter than the input count falls back to undefined */
    {
        const r = Iterator.zip([[1, 2], [9]], { mode: "longest", padding: [] }).toArray();
        eq(r[1][1], undefined, "short padding falls back to undefined");
    }
    /* explicit default mode behaves as shortest */
    eqJSON(Iterator.zip([[1, 2, 3], ["a"]], { mode: "shortest" }).toArray(),
           [[1, "a"]], 'mode:"shortest" is the default behaviour');
    /* strict: equal is fine, unequal throws */
    eqJSON(Iterator.zip([[1, 2], ["a", "b"]], { mode: "strict" }).toArray(),
           [[1, "a"], [2, "b"]], "strict accepts equal lengths");
    throwsType(() => Iterator.zip([[1, 2, 3], ["a", "b"]], { mode: "strict" }).toArray(),
               "strict rejects a longer first input");
    throwsType(() => Iterator.zip([[1, 2], ["a", "b", "c"]], { mode: "strict" }).toArray(),
               "strict rejects a longer second input");
    /* strict with everything empty is still equal */
    eqJSON(Iterator.zip([[], []], { mode: "strict" }).toArray(), [], "strict accepts all-empty");
}

/* ==================================================================== *
 *  Iterator.zip -- CLOSING, which is the part that leaks if wrong
 * ==================================================================== */
{
    /* shortest: the inputs that had not finished must be closed */
    const a = counted([1, 2, 3, 4, 5]);
    const b = counted(["x", "y"]);
    Iterator.zip([a.iter, b.iter]).toArray();
    eq(a.closed, 1, "shortest closes the input that still had values");
    eq(b.closed, 0, "the exhausted input is not closed again");

    /* an infinite input must not hang, and must be closed */
    const inf = counted(null, { infinite: true });
    const fin = counted(["p", "q"]);
    eqJSON(Iterator.zip([inf.iter, fin.iter]).toArray(),
           [[0, "p"], [1, "q"]], "infinite x finite terminates");
    eq(inf.closed, 1, "the infinite input is closed");

    /* explicit .return() closes every live input */
    const c = counted([1, 2, 3]), d = counted([4, 5, 6]);
    const z = Iterator.zip([c.iter, d.iter]);
    z.next();
    z.return();
    eq(c.closed, 1, "return() closes input 0");
    eq(d.closed, 1, "return() closes input 1");
    eqJSON(z.toArray(), [], "a returned zip is done");

    /* strict's throw must also close the inputs rather than abandoning them */
    const e = counted([1, 2, 3]), f = counted(["a"]);
    try { Iterator.zip([e.iter, f.iter], { mode: "strict" }).toArray(); } catch (_) { /* expected */ }
    eq(e.closed, 1, "a strict mismatch still closes the unfinished input");

    /* breaking out of for..of closes everything */
    const g = counted([1, 2, 3, 4]), h = counted([5, 6, 7, 8]);
    for (const _ of Iterator.zip([g.iter, h.iter])) break;
    eq(g.closed, 1, "break closes input 0");
    eq(h.closed, 1, "break closes input 1");
}

/* ==================================================================== *
 *  Iterator.zipKeyed
 * ==================================================================== */
{
    eqJSON(Iterator.zipKeyed({ a: [1, 2], b: ["x", "y"] }).toArray(),
           [{ a: 1, b: "x" }, { a: 2, b: "y" }], "keyed tuples are objects");
    eqJSON(Iterator.zipKeyed({}).toArray(), [], "no keys -> immediately done");
    eqJSON(Iterator.zipKeyed({ a: [1, 2, 3], b: ["x"] }).toArray(),
           [{ a: 1, b: "x" }], "keyed shortest");
    {
        const r = Iterator.zipKeyed({ a: [1, 2], b: ["x"] }, { mode: "longest" }).toArray();
        assert("b" in r[1], "a padded key is PRESENT on the object");
        eq(r[1].b, undefined, "and holds undefined");
    }
    eqJSON(Iterator.zipKeyed({ a: [1], b: ["x"] }, { mode: "strict" }).toArray(),
           [{ a: 1, b: "x" }], "keyed strict accepts equal lengths");
    throwsType(() => Iterator.zipKeyed({ a: [1, 2], b: ["x"] }, { mode: "strict" }).toArray(),
               "keyed strict rejects unequal lengths");
    /* only OWN ENUMERABLE keys participate */
    {
        const proto = { inherited: [9, 9] };
        const o = Object.create(proto);
        o.own = [1, 2];
        Object.defineProperty(o, "hidden", { value: [7, 7], enumerable: false });
        const r = Iterator.zipKeyed(o).toArray();
        eqJSON(r, [{ own: 1 }, { own: 2 }], "inherited and non-enumerable keys are ignored");
    }
    /* keyed closing behaves like the array form */
    {
        const a = counted([1, 2, 3]), b = counted(["x"]);
        Iterator.zipKeyed({ a: a.iter, b: b.iter }).toArray();
        eq(a.closed, 1, "keyed shortest closes the unfinished input");
    }
}

/* ==================================================================== *
 *  Iterator.zip -- rejections
 * ==================================================================== */
{
    throwsType(() => Iterator.zip("abc"), "a string is not accepted as the input list");
    throwsType(() => Iterator.zip([[1], "ab"]), "a string is not accepted as an input");
    throwsType(() => Iterator.zip(null), "null is rejected");
    throwsType(() => Iterator.zip(undefined), "undefined is rejected");
    throwsType(() => Iterator.zip(42), "a number is rejected");
    throwsType(() => Iterator.zip([[1]], { mode: "nope" }), "an unknown mode is rejected");
    throwsType(() => Iterator.zip([[1]], 42), "a non-object options is rejected");
    throwsType(() => Iterator.zip([1, 2]), "non-iterable inputs are rejected");
    /* options are read ONCE, at construction: a mode getter cannot flip
     * behaviour part-way through */
    {
        let reads = 0;
        const opts = { get mode() { reads++; return "longest"; } };
        Iterator.zip([[1, 2], ["a"]], opts).toArray();
        eq(reads, 1, "mode is read exactly once, at construction");
    }
}

/* ==================================================================== *
 *  composition with the rest of the helper surface
 * ==================================================================== */
{
    const z = Iterator.zip([[1, 2, 3], [10, 20, 30]]);
    assert(z instanceof Iterator, "a zip is an Iterator");
    eqJSON(z.map(([a, b]) => a + b).toArray(), [11, 22, 33], "zip -> map");
    eqJSON(Iterator.zip([[1, 2, 3, 4], [1, 0, 1, 0]])
             .filter(([, keep]) => keep).map(([v]) => v).toArray(),
           [1, 3], "zip -> filter -> map");
    eq(Iterator.zip([[1, 2, 3], [4, 5, 6]])
         .reduce((acc, [a, b]) => acc + a * b, 0), 32, "zip -> reduce");
    eqJSON(Iterator.zip([[1, 2, 3, 4], ["a", "b", "c", "d"]]).take(2).toArray(),
           [[1, "a"], [2, "b"]], "zip -> take");
    /* take() must close the zip, which must close the inputs */
    {
        const a = counted([1, 2, 3, 4]), b = counted([5, 6, 7, 8]);
        Iterator.zip([a.iter, b.iter]).take(1).toArray();
        eq(a.closed, 1, "take closes through the zip to input 0");
        eq(b.closed, 1, "take closes through the zip to input 1");
    }
    /* zip of zips */
    eqJSON(Iterator.zip([Iterator.zip([[1], [2]]), [[3, 4]]]).toArray(),
           [[[1, 2], [3, 4]]], "a zip can be an input to a zip");
}

print("test_iterator_gap: all " + n + " tests passed");
