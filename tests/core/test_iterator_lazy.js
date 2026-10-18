let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function eq(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error("assertion failed: " + msg +
                        "\n  got:  " + JSON.stringify(got) +
                        "\n  want: " + JSON.stringify(want));
}
function eqJson(got, want, msg) {
    n++;
    const a = JSON.stringify(got), b = JSON.stringify(want);
    if (a !== b)
        throw new Error("assertion failed: " + msg + "\n  got:  " + a + "\n  want: " + b);
}

const CORPORA = [
    [],
    [1],
    [1, 2, 3, 4, 5, 6],
    [3, 1, 4, 1, 5, 9, 2, 6],
    [1, 1, 2, 2, 2, 3, 1],
    [0, -1, 2.5, 100, -100],
];

const INTERMEDIATE = [
    ["takeWhile", [(x) => x < 4]],
    ["takeWhile", [(x) => true]],
    ["takeWhile", [(x) => false]],
    ["dropWhile", [(x) => x < 4]],
    ["dropWhile", [(x) => false]],
    ["scan", [(a, b) => a + b, 0]],
    ["scan", [(a, b) => a * b, 1]],
    ["intersperse", ["|"]],
    ["compact", []],
    ["dropRepeats", []],
    ["dropRepeatsWith", [(a, b) => a === b]],
    ["dropRepeatsBy", [(x) => x % 2]],
    ["aperture", [1]],
    ["aperture", [2]],
    ["aperture", [3]],
    ["splitEvery", [2]],
    ["splitEvery", [3]],
    ["splitEvery", [100]],
    ["zipWith", [(a, b) => a + b, [10, 20, 30]]],
    ["zipWith", [(a, b) => [a, b], []]],
    ["init", []],
    ["tail", []],
    ["unique", []],
    ["unique", [(x) => x % 3]],
];

for (const xs of CORPORA) {
    for (const [name, args] of INTERMEDIATE) {
        const eager = xs[name](...args);
        const lazy = xs.lazy()[name](...args).toArray();
        eqJson(lazy, eager, "lazy " + name + "(" + args.length + ") on [" + xs + "]");
    }
}

eqJson([1, null, 2, undefined, 3, null].lazy().compact().toArray(),
       [1, null, 2, undefined, 3, null].compact(), "compact with nullish");

const RECS = [{ id: 1, v: "a" }, { id: 2, v: "b" }, { id: 3, v: "c" }];
eqJson(RECS.lazy().pluck("id").toArray(), RECS.pluck("id"), "pluck");

const TERMINALS = [
    ["sum", []], ["average", []], ["mean", []], ["product", []],
    ["min", []], ["max", []], ["min", [(x) => -x]], ["max", [(x) => -x]],
    ["none", [1]], ["any", [1]], ["all", [(x) => x > 0]], ["all", [(x) => x < 99]],
    ["count", []], ["count", [(x) => x > 2]], ["count", [1]],
    ["last", []], ["first", []], ["head", []],
    ["countBy", [(x) => x % 2]], ["indexBy", [(x) => x % 2]], ["groupBy", [(x) => x % 2]],
    ["reduceBy", [(a, x) => a + x, 0, (x) => x % 2]],
];
for (const xs of CORPORA) {
    for (const [name, args] of TERMINALS) {
        eqJson(xs.lazy()[name](...args), xs[name](...args),
               "lazy terminal " + name + " on [" + xs + "]");
    }
    eq(xs.lazy().nth(2), xs[2], "nth on [" + xs + "]");
    eq(xs.lazy().findIndex((x) => x > 2), xs.findIndex((x) => x > 2),
       "findIndex on [" + xs + "]");
}

eqJson(["ab", "cd", "ae"].lazy().takeWhile(/a/).toArray(),
       ["ab", "cd", "ae"].takeWhile(/a/), "takeWhile with a RegExp");
eq(["ab", "cd", "ae"].lazy().count(/a/), ["ab", "cd", "ae"].count(/a/),
   "count with a RegExp");

eqJson([1, 2, 3, 4, 5, 6, 7, 8].lazy().filter((x) => x % 2).map((x) => x * 10)
           .takeWhile((x) => x < 60).toArray(),
       [10, 30, 50], "compose: filter -> map -> takeWhile");
eqJson([1, 2, 3, 4].lazy().scan((a, b) => a + b, 0).drop(1).toArray(),
       [1, 3, 6, 10], "compose: scan -> drop");

function counting(limit) {
    const st = { pulls: 0, closed: 0 };
    let i = 0;
    st.iter = {
        next() {
            st.pulls++;
            return i < limit ? { value: i++, done: false }
                             : { value: undefined, done: true };
        },
        return() {
            st.closed++;
            return { done: true };
        },
        [Symbol.iterator]() { return this; },
    };
    return st;
}

{
    const st = counting(1e9);
    let calls = 0;
    const out = Iterator.from(st.iter).map((x) => { calls++; return x * 2; })
                    .take(3).toArray();
    eqJson(out, [0, 2, 4], "take(3) result");
    eq(st.pulls, 3, "take(3) pulls exactly 3 from an unbounded source");
    eq(calls, 3, "take(3) calls the mapper exactly 3 times");
}
{
    const st = counting(1e9);
    const out = Iterator.from(st.iter).takeWhile((x) => x < 3).toArray();
    eqJson(out, [0, 1, 2], "takeWhile result");
    eq(st.pulls, 4, "takeWhile pulls one past the last accepted element");
    eq(st.closed, 1, "takeWhile closes the source exactly once");
}
{
    const st = counting(1e9);
    eq(Iterator.from(st.iter).first(), 0, "first() value");
    eq(st.pulls, 1, "first() pulls exactly one");
    eq(st.closed, 1, "first() closes the source");
}
{
    const st = counting(1e9);
    eq(Iterator.from(st.iter).nth(4), 4, "nth(4) value");
    eq(st.pulls, 5, "nth(4) pulls exactly five");
    eq(st.closed, 1, "nth() closes the source");
}
{
    const st = counting(1e9);
    eq(Iterator.from(st.iter).findIndex((x) => x === 2), 2, "findIndex value");
    eq(st.pulls, 3, "findIndex stops at the match");
    eq(st.closed, 1, "findIndex closes the source");
}
{
    const st = counting(1e9);
    eq(Iterator.from(st.iter).any((x) => x === 1), true, "any() value");
    eq(st.pulls, 2, "any() stops at the match");
    eq(st.closed, 1, "any() closes the source");
}
{
    const st = counting(1e9);
    eq(Iterator.from(st.iter).all((x) => x < 2), false, "all() value");
    eq(st.pulls, 3, "all() stops at the counterexample");
    eq(st.closed, 1, "all() closes the source");
}
{
    const st = counting(10);
    const chain = Iterator.from(st.iter).map((x) => x).takeWhile((x) => true)
                      .unique().aperture(2);
    eq(st.pulls, 0, "building a pipeline pulls nothing");
    chain.next();
    eq(st.pulls, 2, "the first aperture(2) window pulls exactly the window");
    chain.next();
    eq(st.pulls, 3, "each later window pulls exactly one more");
}
{
    const st = counting(10);
    const chain = Iterator.from(st.iter).splitEvery(4);
    eqJson(chain.next().value, [0, 1, 2, 3], "first chunk");
    eq(st.pulls, 4, "splitEvery pulls exactly one chunk");
}

{
    const st = counting(10);
    const h = Iterator.from(st.iter).dropWhile((x) => false);
    h.next();
    h.return();
    eq(st.closed, 1, "helper.return() closes the source once");
    eq(h.next().done, true, "a returned helper is done");
    eq(st.closed, 1, "a second next() does not re-close");
}
{
    const st = counting(1e9);
    let seen = 0;
    for (const x of Iterator.from(st.iter).scan((a, b) => a + b, 0)) {
        if (++seen === 3) break;
    }
    eq(st.closed, 1, "break out of for..of closes the source exactly once");
}
{
    const st = counting(10);
    let threw = null;
    try {
        Iterator.from(st.iter).scan(() => { throw new Error("boom"); }, 0).toArray();
    } catch (e) { threw = e.message; }
    eq(threw, "boom", "the callback's exception propagates");
    eq(st.closed, 1, "a throwing callback closes the source exactly once");
}
{
    const bad = {
        next() { throw new Error("src"); },
        return() { throw new Error("must not be called"); },
        [Symbol.iterator]() { return this; },
    };
    let threw = null;
    try { Iterator.from(bad).unique().toArray(); } catch (e) { threw = e.message; }
    eq(threw, "src", "a source throwing in next() propagates, without closing");
}
{
    const st = counting(10);
    st.iter.return = function () { throw new Error("close-fail"); };
    let threw = null;
    try {
        Iterator.from(st.iter).takeWhile((x) => x < 2).toArray();
    } catch (e) { threw = e.message; }
    eq(threw, "close-fail", "a throwing return() surfaces");
}
{
    const a = counting(2), b = counting(1e9);
    Iterator.from(a.iter).zipWith((x, y) => x + y, b.iter).toArray();
    eq(b.closed, 1, "zipWith closes the partner when the primary ends");
    const c = counting(1e9), d = counting(2);
    Iterator.from(c.iter).zipWith((x, y) => x + y, d.iter).toArray();
    eq(c.closed, 1, "zipWith closes the primary when the partner ends");
}

{
    let h = null, threw = null;
    h = [1, 2, 3].lazy().scan((a, b) => {
        try { h.next(); } catch (e) { threw = e.constructor.name; }
        return a + b;
    }, 0);
    h.toArray();
    eq(threw, "TypeError", "re-entering a running lazy helper throws TypeError");
}

function throwsRange(fn, msg) {
    n++;
    try { fn(); } catch (e) {
        if (e instanceof RangeError) return;
        throw new Error("assertion failed: " + msg + " threw " + e);
    }
    throw new Error("assertion failed: " + msg + " did not throw");
}
throwsRange(() => [1, 2].lazy().aperture(0), "aperture(0)");
throwsRange(() => [1, 2].lazy().aperture(-1), "aperture(-1)");
throwsRange(() => [1, 2].lazy().splitEvery(0), "splitEvery(0)");
throwsRange(() => [1, 2].lazy().nth(-1), "nth(-1) needs the end");
throwsRange(() => [1, 2].lazy().tee(0), "tee(0)");
throwsRange(() => [1, 2].lazy().tee(2000), "tee(2000)");

{
    const [a, b] = [1, 2, 3, 4].lazy().tee();
    eqJson(a.toArray(), [1, 2, 3, 4], "tee branch A drained first");
    eqJson(b.toArray(), [1, 2, 3, 4], "tee branch B sees the buffered items");
}
{
    const [a, b, c] = [1, 2, 3].lazy().tee(3);
    const got = [];
    for (let i = 0; i < 3; i++) {
        got.push(a.next().value, b.next().value, c.next().value);
    }
    eqJson(got, [1, 1, 1, 2, 2, 2, 3, 3, 3], "tee(3) interleaved");
    eq(a.next().done, true, "tee branch A ends");
    eq(b.next().done, true, "tee branch B ends");
    eq(c.next().done, true, "tee branch C ends");
}
{
    const st = counting(1e9);
    const [a, b] = Iterator.from(st.iter).tee();
    a.next(); a.next(); a.next();
    eq(st.pulls, 3, "tee pulls once per distinct element, not once per branch");
    b.next();
    eq(st.pulls, 3, "the lagging branch reads from the queue");
    a.return();
    eq(st.closed, 0, "a tee branch's return() does not close the shared source");
    eq(b.next().value, 1, "the surviving branch keeps reading");
}

const rounds = +(scriptArgs[1] || 0);
if (rounds > 0) {
    function rss() {
        return typeof std !== "undefined" ? 0 : 0;
    }
    for (const N of [1e5, 1e6, 1e7]) {
        function* upto(k) { for (let i = 0; i < k; i++) yield i; }
        let acc = 0;
        const t0 = Date.now();
        for (const v of Iterator.from(upto(N)).map((x) => x + 1)
                            .filter((x) => (x & 1023) === 0).aperture(4)) {
            acc += v[0];
        }
        console.log("N=" + N + " acc=" + acc + " ms=" + (Date.now() - t0) +
                    " -- watch peak RSS externally; it must not grow with N");
    }
}

console.log("test_iterator_lazy.js: " + n + " assertions passed");
