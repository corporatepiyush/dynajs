/* Modern ECMAScript (ES2023..ES2026 track) + runtime globals test suite.
   Style follows tests/test_builtin.js; coverage breadth inspired by the
   Bun runtime test suite, adapted to the dynajs shell (no external harness). */
"use strict";

function assert(actual, expected, message) {
    if (arguments.length === 1)
        expected = true;
    if (typeof actual === typeof expected) {
        if (actual === expected) {
            if (actual !== 0 || (1 / actual) === (1 / expected))
                return;
        }
        if (typeof actual === 'number' && isNaN(actual) && isNaN(expected))
            return;
        if (typeof actual === 'object') {
            if (actual !== null && expected !== null
            &&  actual.constructor === expected.constructor
            &&  actual.toString() === expected.toString())
                return;
        }
    }
    throw Error("assertion failed: got |" + actual + "|" +
                ", expected |" + expected + "|" +
                (message ? " (" + message + ")" : ""));
}

function assert_throws(expected_error, func, message) {
    var err = false;
    try {
        func();
    } catch (e) {
        err = true;
        if (!(e instanceof expected_error))
            throw Error("unexpected exception type: " + e +
                        (message ? " (" + message + ")" : ""));
    }
    if (!err)
        throw Error("expected exception" +
                    (message ? " (" + message + ")" : ""));
}

function assert_array_equals(a, b, message) {
    assert(Array.isArray(a), true, message);
    assert(a.length, b.length, message);
    for (var i = 0; i < a.length; i++)
        assert(a[i], b[i], message);
}

/*---- ES2023: change array by copy + findLast ----*/

function test_array_es2023() {
    var a = [3, 1, 2];
    assert_array_equals(a.toSorted(), [1, 2, 3]);
    assert_array_equals(a.toReversed(), [2, 1, 3]);
    assert_array_equals(a.with(1, 9), [3, 9, 2]);
    assert_array_equals(a.with(-1, 9), [3, 1, 9]);
    assert_array_equals(a.toSpliced(1, 1, 7, 8), [3, 7, 8, 2]);
    assert_array_equals(a, [3, 1, 2], "originals untouched");
    assert_throws(RangeError, () => a.with(3, 0));
    assert_throws(RangeError, () => a.with(-4, 0));
    assert(a.findLast(x => x < 3), 2);
    assert(a.findLastIndex(x => x < 3), 2);
    assert([].findLast(() => true), undefined);
    /* sparse arrays are densified by the copying methods */
    var s = [1, , 3];
    assert(1 in s.toReversed(), true);
    assert(s.toReversed()[1], undefined);
    /* typed array variants */
    var t = new Int32Array([3, 1, 2]);
    assert(t.toSorted().join(","), "1,2,3");
    assert(t.with(0, 5).join(","), "5,1,2");
    assert(t.join(","), "3,1,2");
}

/*---- ES2024: Array.fromAsync ----*/

async function test_from_async() {
    async function* agen() { yield 1; yield Promise.resolve(2); yield 3; }
    assert_array_equals(await Array.fromAsync(agen()), [1, 2, 3]);

    /* sync iterable: values awaited through async-from-sync wrapping */
    assert_array_equals(await Array.fromAsync([Promise.resolve(4), 5]), [4, 5]);
    assert_array_equals(await Array.fromAsync("ab"), ["a", "b"]);
    assert_array_equals(await Array.fromAsync(new Set([1, 2])), [1, 2]);

    /* array-like: each element awaited */
    assert_array_equals(
        await Array.fromAsync({ length: 3, 0: Promise.resolve("x"), 1: "y", 2: "z" }),
        ["x", "y", "z"]);
    assert_array_equals(await Array.fromAsync({ length: 0 }), []);
    /* array-like holes become undefined, length is respected */
    var hole = await Array.fromAsync({ length: 2, 0: 7 });
    assert(hole.length, 2);
    assert(hole[1], undefined);

    /* mapfn: receives (value, index), result awaited */
    assert_array_equals(
        await Array.fromAsync([10, 20], async (v, i) => v + i),
        [10, 21]);
    /* mapfn on array-like path */
    assert_array_equals(
        await Array.fromAsync({ length: 2, 0: 1, 1: 2 }, v => v * 2),
        [2, 4]);

    /* subclass construction */
    class MyArr extends Array {}
    var m = await Array.fromAsync.call(MyArr, [1]);
    assert(m instanceof MyArr, true);
    assert(m.length, 1);

    /* function metadata */
    assert(Array.fromAsync.length, 1);
    assert(Array.fromAsync.name, "fromAsync");

    /* rejections */
    var caught = null;
    try { await Array.fromAsync(null); } catch (e) { caught = e; }
    assert(caught instanceof TypeError, true, "null input rejects");
    caught = null;
    try { await Array.fromAsync([1], "nope"); } catch (e) { caught = e; }
    assert(caught instanceof TypeError, true, "bad mapfn rejects");
    caught = null;
    try {
        await Array.fromAsync((async function* () { throw Error("gen"); })());
    } catch (e) { caught = e; }
    assert(caught instanceof Error && caught.message === "gen", true);

    /* mapper throw closes the async iterator */
    var closed = false;
    var it = {
        [Symbol.asyncIterator]() {
            return {
                next: () => Promise.resolve({ value: 1, done: false }),
                return() { closed = true; return Promise.resolve({ done: true }); },
            };
        }
    };
    caught = null;
    try { await Array.fromAsync(it, () => { throw Error("boom"); }); }
    catch (e) { caught = e; }
    assert(caught && caught.message, "boom");
    assert(closed, true, "iterator closed on mapper throw");

    /* rejected element promise from a sync iterable rejects the result */
    caught = null;
    try { await Array.fromAsync([Promise.reject(Error("el"))]); }
    catch (e) { caught = e; }
    assert(caught && caught.message, "el");

    /* constructor throwing on the iterator path must reject WITHOUT
       calling iterator.return() (Construct is a plain ReturnIfAbrupt) */
    var returned = false;
    var iter2 = {
        [Symbol.asyncIterator]() {
            return {
                next: () => Promise.resolve({ value: 1, done: false }),
                return() { returned = true; return Promise.resolve({ done: true }); },
            };
        }
    };
    function BadCtor() { throw Error("ctor"); }
    caught = null;
    try { await Array.fromAsync.call(BadCtor, iter2); }
    catch (e) { caught = e; }
    assert(caught && caught.message, "ctor");
    assert(returned, false, "iterator NOT closed on constructor throw");
}

/*---- ES2024: Promise.withResolvers, resizable ArrayBuffer ----*/

function test_promise_withresolvers() {
    var w = Promise.withResolvers();
    assert(typeof w.resolve, "function");
    assert(typeof w.reject, "function");
    assert(w.promise instanceof Promise, true);
    var got = null;
    w.promise.then(v => { got = v; });
    w.resolve(42);
    return w.promise.then(() => assert(got, 42));
}

function test_resizable_arraybuffer() {
    var ab = new ArrayBuffer(4, { maxByteLength: 16 });
    assert(ab.resizable, true);
    assert(ab.maxByteLength, 16);
    var view = new Uint8Array(ab); /* length-tracking */
    assert(view.length, 4);
    ab.resize(8);
    assert(ab.byteLength, 8);
    assert(view.length, 8, "length-tracking view grows");
    ab.resize(2);
    assert(view.length, 2, "length-tracking view shrinks");
    assert_throws(RangeError, () => ab.resize(32));

    var fixed = new ArrayBuffer(4);
    assert(fixed.resizable, false);
    assert_throws(TypeError, () => fixed.resize(8));

    /* transfer detaches the source */
    var src = new ArrayBuffer(4);
    new Uint8Array(src)[0] = 99;
    var dst = src.transfer(8);
    assert(src.detached, true);
    assert(dst.byteLength, 8);
    assert(new Uint8Array(dst)[0], 99);
    var dst2 = dst.transferToFixedLength();
    assert(dst2.resizable, false);

    /* growable SharedArrayBuffer */
    var sab = new SharedArrayBuffer(4, { maxByteLength: 8 });
    assert(sab.growable, true);
    sab.grow(8);
    assert(sab.byteLength, 8);
    assert_throws(RangeError, () => sab.grow(4), "SAB cannot shrink");
}

/*---- ES2024: String well-formedness, groupBy ----*/

function test_wellformed() {
    var lone = "a\uD800b";
    assert(lone.isWellFormed(), false);
    assert("ok".isWellFormed(), true);
    assert(lone.toWellFormed(), "a�b");
    assert("😀".isWellFormed(), true, "paired surrogates ok");
}

function test_groupby() {
    var res = Object.groupBy([1, 2, 3, 4], v => v % 2 ? "odd" : "even");
    assert_array_equals(res.odd, [1, 3]);
    assert_array_equals(res.even, [2, 4]);
    assert(Object.getPrototypeOf(res), null, "groupBy returns null-proto object");
    var m = Map.groupBy([1, 2, 3], v => v % 2);
    assert_array_equals(m.get(1), [1, 3]);
    assert_array_equals(m.get(0), [2]);
}

/*---- ES2025: iterator helpers ----*/

function test_iterator_helpers() {
    assert(typeof Iterator, "function");
    var r = [1, 2, 3, 4, 5].values()
        .map(x => x * 2)
        .filter(x => x > 4)
        .take(2)
        .toArray();
    assert_array_equals(r, [6, 8]);
    assert_array_equals([1, 2, 3].values().drop(1).toArray(), [2, 3]);
    assert_array_equals([[1, 2], [3]].values().flatMap(x => x).toArray(), [1, 2, 3]);
    assert([1, 2, 3].values().reduce((a, b) => a + b, 0), 6);
    assert([1, 2, 3].values().find(x => x > 1), 2);
    assert([1, 2, 3].values().some(x => x === 3), true);
    assert([1, 2, 3].values().every(x => x > 0), true);

    /* laziness: helpers must not pre-consume the source */
    var pulled = 0;
    function* counting() { for (;;) { pulled++; yield pulled; } }
    var it = counting()[Symbol.iterator]().map(x => x).take(3);
    assert(pulled, 0, "no eager pull");
    assert_array_equals(it.toArray(), [1, 2, 3]);
    assert(pulled, 3, "pulled exactly 3");

    /* Iterator.from wraps plain iterators */
    var plain = { i: 0, next() { return { value: this.i++, done: this.i > 2 }; } };
    assert_array_equals(Iterator.from(plain).toArray(), [0, 1]);
    assert_throws(RangeError, () => [].values().take(-1));
    /* closing a helper chain over an iterator without 'return'
       (regression: helper close previously threw TypeError) */
    assert_array_equals(
        [1, 2, 3, 4, 5].values().map(x => x * 2).filter(x => x > 4).take(2).toArray(),
        [6, 8]);
    var h = [1, 2, 3].values().map(x => x);
    assert(h.return().done, true);
    assert(h.next().done, true, "helper closed after return()");
}

/*---- ES2025: Set methods ----*/

function test_set_methods() {
    var a = new Set([1, 2, 3]);
    var b = new Set([2, 3, 4]);
    assert_array_equals([...a.union(b)], [1, 2, 3, 4]);
    assert_array_equals([...a.intersection(b)], [2, 3]);
    assert_array_equals([...a.difference(b)], [1]);
    assert_array_equals([...a.symmetricDifference(b)], [1, 4]);
    assert(a.isSubsetOf(new Set([1, 2, 3, 9])), true);
    assert(a.isSupersetOf(new Set([1, 2])), true);
    assert(a.isDisjointFrom(new Set([7, 8])), true);
    assert(a.isDisjointFrom(b), false);
    /* set-like argument (has/keys/size protocol) */
    var setlike = {
        size: 2,
        has: v => v === 1 || v === 9,
        keys: () => [1, 9].values(),
    };
    assert_array_equals([...a.intersection(setlike)], [1]);
    assert(a.size, 3, "receivers unchanged");
}

/*---- ES2025: Promise.try, RegExp.escape, regexp features ----*/

function test_promise_try() {
    var p1 = Promise.try(() => 7);
    var p2 = Promise.try(() => { throw Error("sync"); });
    var p3 = Promise.try((a, b) => a + b, 1, 2);
    return p1.then(v => {
        assert(v, 7);
        return p2.then(() => assert(false), e => assert(e.message, "sync"));
    }).then(() => p3.then(v => assert(v, 3)));
}

function test_regexp_modern() {
    /* escape */
    var esc = RegExp.escape("a.b*c");
    assert(new RegExp("^" + esc + "$").test("a.b*c"), true);
    assert(RegExp.escape("(){}"), "\\(\\)\\{\\}");
    /* v flag: set difference + properties */
    var re = new RegExp("^[\\p{L}--[a-z]]+$", "v");
    assert(re.unicodeSets, true);
    assert(re.test("AB"), true);
    assert(re.test("ab"), false);
    /* inline modifiers */
    assert(new RegExp("(?i:abc)d").test("ABCd"), true);
    assert(new RegExp("(?i:abc)d").test("ABCD"), false);
    /* duplicate named groups across alternatives */
    var dup = new RegExp("(?<x>a)|(?<x>b)");
    assert(dup.exec("b").groups.x, "b");
    assert(dup.exec("a").groups.x, "a");
    /* d flag indices */
    var d = /b(c)/d.exec("abc");
    assert(d.indices[0][0], 1);
    assert(d.indices[1][0], 2);
    assert(/a/d.hasIndices, true);
}

/*---- ES2025: Float16, Math.sumPrecise/f16round ----*/

function test_float16() {
    var f = new Float16Array([1.337]);
    assert(f[0], Math.f16round(1.337));
    assert(Math.f16round(65520), Infinity, "f16 overflow");
    var dv = new DataView(new ArrayBuffer(2));
    dv.setFloat16(0, 1.5);
    assert(dv.getFloat16(0), 1.5);
    assert(Float16Array.BYTES_PER_ELEMENT, 2);
}

function test_sum_precise() {
    assert(Math.sumPrecise([1e100, 1, -1e100]), 1, "no cancellation loss");
    assert(Math.sumPrecise([]), -0, "empty sum is -0");
    assert(Math.sumPrecise([0.1, 0.2]), 0.1 + 0.2);
    assert(Math.sumPrecise([2 ** 53, 1, 1]), 2 ** 53 + 2);
    assert_throws(TypeError, () => Math.sumPrecise([1n]));
}

/*---- ES2025: Uint8Array base64 / hex ----*/

function test_uint8_base64_hex() {
    var bytes = new Uint8Array([72, 101, 108, 108, 111]);
    assert(bytes.toBase64(), "SGVsbG8=");
    assert(bytes.toHex(), "48656c6c6f");
    assert_array_equals([...Uint8Array.fromBase64("SGVsbG8=")], [...bytes]);
    assert_array_equals([...Uint8Array.fromHex("48656c6c6f")], [...bytes]);
    /* base64url alphabet */
    var url = new Uint8Array([251, 255]);
    assert(url.toBase64({ alphabet: "base64url" }), "-_8=");
    assert(url.toBase64({ omitPadding: true }), url.toBase64().replace(/=+$/, ""));
    /* setFromBase64 partial write into a small target */
    var target = new Uint8Array(3);
    var res = target.setFromBase64("SGVsbG8=");
    assert(res.written <= 3, true);
    assert(target[0], 72);
    /* errors */
    assert_throws(SyntaxError, () => Uint8Array.fromHex("abc"), "odd length");
    assert_throws(SyntaxError, () => Uint8Array.fromHex("zz"));
    assert_throws(SyntaxError, () => Uint8Array.fromBase64("a", { lastChunkHandling: "strict" }));
    assert_throws(TypeError, () => Uint8Array.fromBase64("aa", { alphabet: "wat" }));
}

/*---- ES2026 track: Map/WeakMap upsert, Error.isError ----*/

function test_map_upsert() {
    var m = new Map();
    assert(m.getOrInsert("k", 1), 1);
    assert(m.getOrInsert("k", 2), 1, "existing value wins");
    var calls = 0;
    assert(m.getOrInsertComputed("j", k => { calls++; return k + "!"; }), "j!");
    assert(m.getOrInsertComputed("j", () => { calls++; return "x"; }), "j!");
    assert(calls, 1, "callback only for missing keys");
    var wm = new WeakMap();
    var key = {};
    assert(wm.getOrInsert(key, 9), 9);
    assert(wm.getOrInsert(key, 8), 9);
}

function test_error_modern() {
    assert(Error.isError(new TypeError("x")), true);
    assert(Error.isError({ message: "fake" }), false);
    assert(Error.isError("nope"), false);
    var e = new Error("outer", { cause: "inner" });
    assert(e.cause, "inner");
    var agg = new AggregateError([e], "many");
    assert(agg.errors[0], e);
    assert(agg.message, "many");
}

/*---- ES2023: symbols as WeakMap keys ----*/

function test_symbol_weak_keys() {
    var wm = new WeakMap();
    var s = Symbol("local");
    wm.set(s, 1);
    assert(wm.get(s), 1);
    var ws = new WeakSet();
    ws.add(s);
    assert(ws.has(s), true);
    /* registered symbols are not allowed as weak keys */
    assert_throws(TypeError, () => new WeakMap().set(Symbol.for("reg"), 1));
    assert_throws(TypeError, () => new WeakRef(Symbol.for("reg")));
}

/*---- Atomics.pause ----*/

function test_atomics_pause() {
    assert(Atomics.pause(), undefined);
    assert(Atomics.pause(5), undefined);
    assert_throws(TypeError, () => Atomics.pause(1.5));
    assert_throws(TypeError, () => Atomics.pause("x"));
}

/*---- runtime globals: console, queueMicrotask, timers ----*/

function test_console() {
    var names = ["log", "info", "debug", "trace", "warn", "error", "assert"];
    for (var n of names)
        assert(typeof console[n], "function", "console." + n);
    for (var n of names)
        assert(Object.keys(console).includes(n), true, "enumerable " + n);
    assert(console.assert(true, "not printed"), undefined);
}

function test_queue_microtask() {
    assert(typeof queueMicrotask, "function");
    assert_throws(TypeError, () => queueMicrotask(1));
    return new Promise(resolve => {
        var order = [];
        queueMicrotask(() => order.push("m1"));
        Promise.resolve().then(() => order.push("p"));
        queueMicrotask(() => {
            order.push("m2");
            assert_array_equals(order, ["m1", "p", "m2"], "FIFO job order");
            resolve();
        });
    });
}

function test_timers() {
    assert(typeof setTimeout, "function");
    assert(typeof setInterval, "function");
    assert(typeof clearTimeout, "function");
    assert(typeof clearInterval, "function");
    return new Promise((resolve, reject) => {
        var ticks = 0;
        var iv = setInterval(() => {
            ticks++;
            if (ticks === 3)
                clearInterval(iv);
            if (ticks > 3)
                reject(Error("interval not cleared"));
        }, 1);
        var dead = setTimeout(() => reject(Error("cleared timeout fired")), 5);
        clearTimeout(dead);
        setTimeout(() => {
            if (ticks === 3)
                resolve();
            else
                reject(Error("interval ticks = " + ticks));
        }, 50);
    });
}

/*---- run everything ----*/

test_array_es2023();
test_promise_withresolvers();
test_resizable_arraybuffer();
test_wellformed();
test_groupby();
test_iterator_helpers();
test_set_methods();
test_regexp_modern();
test_float16();
test_sum_precise();
test_uint8_base64_hex();
test_map_upsert();
test_error_modern();
test_symbol_weak_keys();
test_atomics_pause();
test_console();

async function run_async_tests() {
    await test_from_async();
    await test_promise_try();
    await test_queue_microtask();
    await test_timers();
    print("test_modern: all tests passed");
}
run_async_tests().catch(e => {
    print("test_modern FAILED:", e, "\n", e && e.stack);
    throw e;
});
